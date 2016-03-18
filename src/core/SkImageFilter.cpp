/*
 * Copyright 2012 The Android Open Source Project
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkImageFilter.h"
#include "SkImageFilterCacheKey.h"

#include "SkBitmap.h"
#include "SkBitmapDevice.h"
#include "SkChecksum.h"
#include "SkDevice.h"
#include "SkLocalMatrixImageFilter.h"
#include "SkMatrixImageFilter.h"
#include "SkOncePtr.h"
#include "SkReadBuffer.h"
#include "SkRect.h"
#include "SkSpecialImage.h"
#include "SkSpecialSurface.h"
#include "SkTDynamicHash.h"
#include "SkTInternalLList.h"
#include "SkValidationUtils.h"
#include "SkWriteBuffer.h"
#if SK_SUPPORT_GPU
#include "GrContext.h"
#include "GrDrawContext.h"
#include "SkGrPixelRef.h"
#include "SkGr.h"
#endif

#ifdef SK_BUILD_FOR_IOS
  enum { kDefaultCacheSize = 2 * 1024 * 1024 };
#else
  enum { kDefaultCacheSize = 128 * 1024 * 1024 };
#endif

#ifndef SK_IGNORE_TO_STRING
void SkImageFilter::CropRect::toString(SkString* str) const {
    if (!fFlags) {
        return;
    }

    str->appendf("cropRect (");
    if (fFlags & CropRect::kHasLeft_CropEdge) {
        str->appendf("%.2f, ", fRect.fLeft);
    } else {
        str->appendf("X, ");
    }
    if (fFlags & CropRect::kHasTop_CropEdge) {
        str->appendf("%.2f, ", fRect.fTop);
    } else {
        str->appendf("X, ");
    }
    if (fFlags & CropRect::kHasWidth_CropEdge) {
        str->appendf("%.2f, ", fRect.width());
    } else {
        str->appendf("X, ");
    }
    if (fFlags & CropRect::kHasHeight_CropEdge) {
        str->appendf("%.2f", fRect.height());
    } else {
        str->appendf("X");
    }
    str->appendf(") ");
}
#endif

void SkImageFilter::CropRect::applyTo(const SkIRect& imageBounds,
                                      const SkMatrix& ctm,
                                      SkIRect* cropped) const {
    *cropped = imageBounds;
    if (fFlags) {
        SkRect devCropR;
        ctm.mapRect(&devCropR, fRect);
        const SkIRect devICropR = devCropR.roundOut();

        // Compute the left/top first, in case we have to read them to compute right/bottom
        if (fFlags & kHasLeft_CropEdge) {
            cropped->fLeft = devICropR.fLeft;
        }
        if (fFlags & kHasTop_CropEdge) {
            cropped->fTop = devICropR.fTop;
        }
        if (fFlags & kHasWidth_CropEdge) {
            cropped->fRight = cropped->fLeft + devICropR.width();
        }
        if (fFlags & kHasHeight_CropEdge) {
            cropped->fBottom = cropped->fTop + devICropR.height();
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////

static int32_t next_image_filter_unique_id() {
    static int32_t gImageFilterUniqueID;

    // Never return 0.
    int32_t id;
    do {
        id = sk_atomic_inc(&gImageFilterUniqueID) + 1;
    } while (0 == id);
    return id;
}

SkImageFilter::Common::~Common() {
    for (int i = 0; i < fInputs.count(); ++i) {
        SkSafeUnref(fInputs[i]);
    }
}

void SkImageFilter::Common::allocInputs(int count) {
    const size_t size = count * sizeof(SkImageFilter*);
    fInputs.reset(count);
    sk_bzero(fInputs.get(), size);
}

void SkImageFilter::Common::detachInputs(SkImageFilter** inputs) {
    const size_t size = fInputs.count() * sizeof(SkImageFilter*);
    memcpy(inputs, fInputs.get(), size);
    sk_bzero(fInputs.get(), size);
}

bool SkImageFilter::Common::unflatten(SkReadBuffer& buffer, int expectedCount) {
    const int count = buffer.readInt();
    if (!buffer.validate(count >= 0)) {
        return false;
    }
    if (!buffer.validate(expectedCount < 0 || count == expectedCount)) {
        return false;
    }

    this->allocInputs(count);
    for (int i = 0; i < count; i++) {
        if (buffer.readBool()) {
            fInputs[i] = buffer.readImageFilter();
        }
        if (!buffer.isValid()) {
            return false;
        }
    }
    SkRect rect;
    buffer.readRect(&rect);
    if (!buffer.isValid() || !buffer.validate(SkIsValidRect(rect))) {
        return false;
    }

    uint32_t flags = buffer.readUInt();
    fCropRect = CropRect(rect, flags);
    if (buffer.isVersionLT(SkReadBuffer::kImageFilterNoUniqueID_Version)) {

        (void) buffer.readUInt();
    }
    return buffer.isValid();
}

///////////////////////////////////////////////////////////////////////////////////////////////////

SkImageFilter::SkImageFilter(int inputCount, SkImageFilter** inputs, const CropRect* cropRect)
  : fInputCount(inputCount),
    fInputs(new SkImageFilter*[inputCount]),
    fUsesSrcInput(false),
    fCropRect(cropRect ? *cropRect : CropRect(SkRect(), 0x0)),
    fUniqueID(next_image_filter_unique_id()) {
    for (int i = 0; i < inputCount; ++i) {
        if (nullptr == inputs[i] || inputs[i]->usesSrcInput()) {
            fUsesSrcInput = true;
        }
        fInputs[i] = inputs[i];
        SkSafeRef(fInputs[i]);
    }
}

SkImageFilter::~SkImageFilter() {
    for (int i = 0; i < fInputCount; i++) {
        SkSafeUnref(fInputs[i]);
    }
    delete[] fInputs;
    Cache::Get()->purgeByKeys(fCacheKeys.begin(), fCacheKeys.count());
}

SkImageFilter::SkImageFilter(int inputCount, SkReadBuffer& buffer)
  : fUsesSrcInput(false)
  , fUniqueID(next_image_filter_unique_id()) {
    Common common;
    if (common.unflatten(buffer, inputCount)) {
        fCropRect = common.cropRect();
        fInputCount = common.inputCount();
        fInputs = new SkImageFilter* [fInputCount];
        common.detachInputs(fInputs);
        for (int i = 0; i < fInputCount; ++i) {
            if (nullptr == fInputs[i] || fInputs[i]->usesSrcInput()) {
                fUsesSrcInput = true;
            }
        }
    } else {
        fInputCount = 0;
        fInputs = nullptr;
    }
}

void SkImageFilter::flatten(SkWriteBuffer& buffer) const {
    buffer.writeInt(fInputCount);
    for (int i = 0; i < fInputCount; i++) {
        SkImageFilter* input = this->getInput(i);
        buffer.writeBool(input != nullptr);
        if (input != nullptr) {
            buffer.writeFlattenable(input);
        }
    }
    buffer.writeRect(fCropRect.rect());
    buffer.writeUInt(fCropRect.flags());
}

SkSpecialImage* SkImageFilter::filterImage(SkSpecialImage* src, const Context& context,
                                           SkIPoint* offset) const {
    SkASSERT(src && offset);

    uint32_t srcGenID = fUsesSrcInput ? src->uniqueID() : 0;
    const SkIRect srcSubset = fUsesSrcInput ? src->subset() : SkIRect::MakeWH(0, 0);
    Cache::Key key(fUniqueID, context.ctm(), context.clipBounds(), srcGenID, srcSubset);
    if (context.cache()) {
        SkSpecialImage* result = context.cache()->get(key, offset);
        if (result) {
            return SkRef(result);
        }
    }

    SkSpecialImage* result = this->onFilterImage(src, context, offset);
    if (result && context.cache()) {
        context.cache()->set(key, result, *offset);
        SkAutoMutexAcquire mutex(fMutex);
        fCacheKeys.push_back(key);
    }

    return result;
}

bool SkImageFilter::filterImageDeprecated(Proxy* proxy, const SkBitmap& src,
                                          const Context& context,
                                          SkBitmap* result, SkIPoint* offset) const {
    SkASSERT(result);
    SkASSERT(offset);
    uint32_t srcGenID = fUsesSrcInput ? src.getGenerationID() : 0;
    Cache::Key key(fUniqueID, context.ctm(), context.clipBounds(),
                   srcGenID, SkIRect::MakeWH(0, 0));
    if (context.cache()) {
        if (context.cache()->get(key, result, offset)) {
            return true;
        }
    }
    /*
     *  Give the proxy first shot at the filter. If it returns false, ask
     *  the filter to do it.
     */
    if ((proxy && proxy->filterImage(this, src, context, result, offset)) ||
        this->onFilterImageDeprecated(proxy, src, context, result, offset)) {
        if (context.cache()) {
            context.cache()->set(key, *result, *offset);
            SkAutoMutexAcquire mutex(fMutex);
            fCacheKeys.push_back(key);
        }
        return true;
    }
    return false;
}

bool SkImageFilter::filterInputDeprecated(int index, Proxy* proxy, const SkBitmap& src,
                                          const Context& ctx,
                                          SkBitmap* result, SkIPoint* offset) const {
    SkImageFilter* input = this->getInput(index);
    if (!input) {
        return true;
    }

    sk_sp<SkSpecialImage> specialSrc(SkSpecialImage::internal_fromBM(proxy, src));
    if (!specialSrc) {
        return false;
    }

    SkAutoTUnref<SkSpecialImage> tmp(input->onFilterImage(specialSrc.get(),
                                                          this->mapContext(ctx),
                                                          offset));
    if (!tmp) {
        return false;
    }

    return tmp->internal_getBM(result);
}

bool SkImageFilter::filterBounds(const SkIRect& src, const SkMatrix& ctm, SkIRect* dst,
                                 MapDirection direction) const {
    SkASSERT(dst);
    SkIRect bounds;
    if (kReverse_MapDirection == direction) {
        this->onFilterNodeBounds(src, ctm, &bounds, direction);
        return this->onFilterBounds(bounds, ctm, dst, direction);
    } else {
        SkIRect temp;
        if (!this->onFilterBounds(src, ctm, &bounds, direction)) {
            return false;
        }
        this->onFilterNodeBounds(bounds, ctm, &temp, direction);
        this->getCropRect().applyTo(temp, ctm, dst);
        return true;
    }
}

void SkImageFilter::computeFastBounds(const SkRect& src, SkRect* dst) const {
    if (0 == fInputCount) {
        *dst = src;
        return;
    }
    // We can't work directly on dst, since src and dst may alias.
    SkRect combinedBounds;
    if (this->getInput(0)) {
        this->getInput(0)->computeFastBounds(src, &combinedBounds);
    } else {
        combinedBounds = src;
    }
    for (int i = 1; i < fInputCount; i++) {
        SkImageFilter* input = this->getInput(i);
        if (input) {
            SkRect bounds;
            input->computeFastBounds(src, &bounds);
            combinedBounds.join(bounds);
        } else {
            combinedBounds.join(src);
        }
    }
    *dst = combinedBounds;
}

bool SkImageFilter::canComputeFastBounds() const {
    for (int i = 0; i < fInputCount; i++) {
        SkImageFilter* input = this->getInput(i);
        if (input && !input->canComputeFastBounds()) {
            return false;
        }
    }
    return true;
}

bool SkImageFilter::onFilterImageDeprecated(Proxy*, const SkBitmap&, const Context&,
                                            SkBitmap*, SkIPoint*) const {
    // Only classes that now use the new SkSpecialImage-based path will not have
    // onFilterImageDeprecated methods. For those classes we should never be
    // calling this method.
    SkASSERT(0);
    return false;
}

// SkImageFilter-derived classes that do not yet have their own onFilterImage
// implementation convert back to calling the deprecated filterImage method
SkSpecialImage* SkImageFilter::onFilterImage(SkSpecialImage* src, const Context& ctx,
                                             SkIPoint* offset) const {
    SkBitmap srcBM, resultBM;

    if (!src->internal_getBM(&srcBM)) {
        return nullptr;
    }

    // This is the only valid call to the old filterImage path
    if (!this->filterImageDeprecated(src->internal_getProxy(), srcBM, ctx, &resultBM, offset)) {
        return nullptr;
    }

    return SkSpecialImage::internal_fromBM(src->internal_getProxy(), resultBM).release();
}

bool SkImageFilter::canFilterImageGPU() const {
    return this->asFragmentProcessor(nullptr, nullptr, SkMatrix::I(), SkIRect());
}

bool SkImageFilter::filterImageGPUDeprecated(Proxy* proxy, const SkBitmap& src, const Context& ctx,
                                             SkBitmap* result, SkIPoint* offset) const {
#if SK_SUPPORT_GPU
    SkBitmap input = src;
    SkASSERT(fInputCount == 1);
    SkIPoint srcOffset = SkIPoint::Make(0, 0);
    if (!this->filterInputGPUDeprecated(0, proxy, src, ctx, &input, &srcOffset)) {
        return false;
    }
    GrTexture* srcTexture = input.getTexture();
    SkIRect bounds;
    if (!this->applyCropRectDeprecated(ctx, proxy, input, &srcOffset, &bounds, &input)) {
        return false;
    }
    GrContext* context = srcTexture->getContext();

    GrSurfaceDesc desc;
    desc.fFlags = kRenderTarget_GrSurfaceFlag,
    desc.fWidth = bounds.width();
    desc.fHeight = bounds.height();
    desc.fConfig = kRGBA_8888_GrPixelConfig;

    SkAutoTUnref<GrTexture> dst(context->textureProvider()->createApproxTexture(desc));
    if (!dst) {
        return false;
    }

    GrFragmentProcessor* fp;
    offset->fX = bounds.left();
    offset->fY = bounds.top();
    bounds.offset(-srcOffset);
    SkMatrix matrix(ctx.ctm());
    matrix.postTranslate(SkIntToScalar(-bounds.left()), SkIntToScalar(-bounds.top()));
    GrPaint paint;
    if (this->asFragmentProcessor(&fp, srcTexture, matrix, bounds)) {
        SkASSERT(fp);
        paint.addColorFragmentProcessor(fp)->unref();
        paint.setPorterDuffXPFactory(SkXfermode::kSrc_Mode);

        SkAutoTUnref<GrDrawContext> drawContext(context->drawContext(dst->asRenderTarget()));
        if (drawContext) {
            SkRect srcRect = SkRect::Make(bounds);
            SkRect dstRect = SkRect::MakeWH(srcRect.width(), srcRect.height());
            GrClip clip(dstRect);
            drawContext->fillRectToRect(clip, paint, SkMatrix::I(), dstRect, srcRect);

            GrWrapTextureInBitmap(dst, bounds.width(), bounds.height(), false, result);
            return true;
        }
    }
#endif
    return false;
}

bool SkImageFilter::asAColorFilter(SkColorFilter** filterPtr) const {
    SkASSERT(nullptr != filterPtr);
    if (!this->isColorFilterNode(filterPtr)) {
        return false;
    }
    if (nullptr != this->getInput(0) || (*filterPtr)->affectsTransparentBlack()) {
        (*filterPtr)->unref();
        return false;
    }
    return true;
}

bool SkImageFilter::applyCropRect(const Context& ctx, const SkIRect& srcBounds,
                                  SkIRect* dstBounds) const {
    this->onFilterNodeBounds(srcBounds, ctx.ctm(), dstBounds, kForward_MapDirection);
    fCropRect.applyTo(*dstBounds, ctx.ctm(), dstBounds);
    // Intersect against the clip bounds, in case the crop rect has
    // grown the bounds beyond the original clip. This can happen for
    // example in tiling, where the clip is much smaller than the filtered
    // primitive. If we didn't do this, we would be processing the filter
    // at the full crop rect size in every tile.
    return dstBounds->intersect(ctx.clipBounds());
}

bool SkImageFilter::applyCropRectDeprecated(const Context& ctx, Proxy* proxy, const SkBitmap& src,
                                            SkIPoint* srcOffset, SkIRect* bounds,
                                            SkBitmap* dst) const {
    SkIRect srcBounds;
    src.getBounds(&srcBounds);
    srcBounds.offset(*srcOffset);
    SkIRect dstBounds;
    this->onFilterNodeBounds(srcBounds, ctx.ctm(), &dstBounds, kForward_MapDirection);
    fCropRect.applyTo(dstBounds, ctx.ctm(), bounds);
    if (!bounds->intersect(ctx.clipBounds())) {
        return false;
    }

    if (srcBounds.contains(*bounds)) {
        *dst = src;
        return true;
    } else {
        SkAutoTUnref<SkBaseDevice> device(proxy->createDevice(bounds->width(), bounds->height()));
        if (!device) {
            return false;
        }
        SkCanvas canvas(device);
        canvas.clear(0x00000000);
        canvas.drawBitmap(src, srcOffset->x() - bounds->x(), srcOffset->y() - bounds->y());
        *srcOffset = SkIPoint::Make(bounds->x(), bounds->y());
        *dst = device->accessBitmap(false);
        return true;
    }
}

// Return a larger (newWidth x newHeight) copy of 'src' with black padding
// around it.
static sk_sp<SkSpecialImage> pad_image(SkSpecialImage* src,
                                       int newWidth, int newHeight, int offX, int offY) {

    SkImageInfo info = SkImageInfo::MakeN32Premul(newWidth, newHeight);
    sk_sp<SkSpecialSurface> surf(src->makeSurface(info));
    if (!surf) {
        return nullptr;
    }

    SkCanvas* canvas = surf->getCanvas();
    SkASSERT(canvas);

    canvas->clear(0x0);

    src->draw(canvas, offX, offY, nullptr);

    return surf->makeImageSnapshot();
}

SkSpecialImage* SkImageFilter::applyCropRect(const Context& ctx,
                                             SkSpecialImage* src,
                                             SkIPoint* srcOffset,
                                             SkIRect* bounds) const {
    SkIRect srcBounds;
    srcBounds = SkIRect::MakeXYWH(srcOffset->fX, srcOffset->fY, src->width(), src->height());

    SkIRect dstBounds;
    this->onFilterNodeBounds(srcBounds, ctx.ctm(), &dstBounds, kForward_MapDirection);
    fCropRect.applyTo(dstBounds, ctx.ctm(), bounds);
    if (!bounds->intersect(ctx.clipBounds())) {
        return nullptr;
    }

    if (srcBounds.contains(*bounds)) {
        return SkRef(src);
    } else {
        sk_sp<SkSpecialImage> img(pad_image(src,
                                            bounds->width(), bounds->height(),
                                            srcOffset->x() - bounds->x(),
                                            srcOffset->y() - bounds->y()));
        *srcOffset = SkIPoint::Make(bounds->x(), bounds->y());
        return img.release();
    }
}

bool SkImageFilter::onFilterBounds(const SkIRect& src, const SkMatrix& ctm,
                                   SkIRect* dst, MapDirection direction) const {
    if (fInputCount < 1) {
        *dst = src;
        return true;
    }

    SkIRect totalBounds;
    for (int i = 0; i < fInputCount; ++i) {
        SkImageFilter* filter = this->getInput(i);
        SkIRect rect = src;
        if (filter && !filter->filterBounds(src, ctm, &rect, direction)) {
            return false;
        }
        if (0 == i) {
            totalBounds = rect;
        } else {
            totalBounds.join(rect);
        }
    }

    // don't modify dst until now, so we don't accidentally change it in the
    // loop, but then return false on the next filter.
    *dst = totalBounds;
    return true;
}

void SkImageFilter::onFilterNodeBounds(const SkIRect& src, const SkMatrix&,
                                       SkIRect* dst, MapDirection) const {
    *dst = src;
}


SkImageFilter::Context SkImageFilter::mapContext(const Context& ctx) const {
    SkIRect clipBounds;
    this->onFilterNodeBounds(ctx.clipBounds(), ctx.ctm(), &clipBounds,
                             MapDirection::kReverse_MapDirection);
    return Context(ctx.ctm(), clipBounds, ctx.cache());
}

bool SkImageFilter::asFragmentProcessor(GrFragmentProcessor**, GrTexture*,
                                        const SkMatrix&, const SkIRect&) const {
    return false;
}

SkImageFilter* SkImageFilter::CreateMatrixFilter(const SkMatrix& matrix,
                                                 SkFilterQuality filterQuality,
                                                 SkImageFilter* input) {
    return SkMatrixImageFilter::Create(matrix, filterQuality, input);
}

SkImageFilter* SkImageFilter::newWithLocalMatrix(const SkMatrix& matrix) const {
    // SkLocalMatrixImageFilter takes SkImage* in its factory, but logically that parameter
    // is *always* treated as a const ptr. Hence the const-cast here.
    //
    return SkLocalMatrixImageFilter::Create(matrix, const_cast<SkImageFilter*>(this));
}

SkSpecialImage* SkImageFilter::filterInput(int index,
                                           SkSpecialImage* src,
                                           const Context& ctx,
                                           SkIPoint* offset) const {
    SkImageFilter* input = this->getInput(index);
    if (!input) {
        return SkRef(src);
    }

    SkAutoTUnref<SkSpecialImage> result(input->filterImage(src, this->mapContext(ctx), offset));

#if SK_SUPPORT_GPU
    if (src->peekTexture() && result && !result->peekTexture()) {
        // Keep the result on the GPU - this is still required for some
        // image filters that don't support GPU in all cases
        GrContext* context = src->peekTexture()->getContext();
        return result->makeTextureImage(src->internal_getProxy(), context).release();
    }
#endif

    return result.release();
}

#if SK_SUPPORT_GPU

bool SkImageFilter::filterInputGPUDeprecated(int index, SkImageFilter::Proxy* proxy,
                                             const SkBitmap& src, const Context& ctx,
                                             SkBitmap* result, SkIPoint* offset) const {
    SkImageFilter* input = this->getInput(index);
    if (!input) {
        return true;
    }

    sk_sp<SkSpecialImage> specialSrc(SkSpecialImage::internal_fromBM(proxy, src));
    if (!specialSrc) {
        return false;
    }

    SkAutoTUnref<SkSpecialImage> tmp(input->onFilterImage(specialSrc.get(),
                                                          this->mapContext(ctx),
                                                          offset));
    if (!tmp) {
        return false;
    }

    if (!tmp->internal_getBM(result)) {
        return false;
    }

    if (!result->getTexture()) {
        GrContext* context = src.getTexture()->getContext();

        const SkImageInfo info = result->info();
        if (kUnknown_SkColorType == info.colorType()) {
            return false;
        }
        SkAutoTUnref<GrTexture> resultTex(
            GrRefCachedBitmapTexture(context, *result, GrTextureParams::ClampNoFilter()));
        if (!resultTex) {
            return false;
        }
        result->setPixelRef(new SkGrPixelRef(info, resultTex))->unref();
    }

    return true;
}
#endif

namespace {

class CacheImpl : public SkImageFilter::Cache {
public:
    CacheImpl(size_t maxBytes) : fMaxBytes(maxBytes), fCurrentBytes(0) { }
    ~CacheImpl() override {
        SkTDynamicHash<Value, Key>::Iter iter(&fLookup);

        while (!iter.done()) {
            Value* v = &*iter;
            ++iter;
            delete v;
        }
    }
    struct Value {
        Value(const Key& key, const SkBitmap& bitmap, const SkIPoint& offset)
            : fKey(key), fBitmap(bitmap), fOffset(offset) {}
        Value(const Key& key, SkSpecialImage* image, const SkIPoint& offset)
            : fKey(key), fImage(SkRef(image)), fOffset(offset) {}

        Key fKey;
        SkBitmap fBitmap;
        SkAutoTUnref<SkSpecialImage> fImage;
        SkIPoint fOffset;
        static const Key& GetKey(const Value& v) {
            return v.fKey;
        }
        static uint32_t Hash(const Key& key) {
            return SkChecksum::Murmur3(reinterpret_cast<const uint32_t*>(&key), sizeof(Key));
        }
        SK_DECLARE_INTERNAL_LLIST_INTERFACE(Value);
    };

    bool get(const Key& key, SkBitmap* result, SkIPoint* offset) const override {
        SkAutoMutexAcquire mutex(fMutex);
        if (Value* v = fLookup.find(key)) {
            *result = v->fBitmap;
            *offset = v->fOffset;
            if (v != fLRU.head()) {
                fLRU.remove(v);
                fLRU.addToHead(v);
            }
            return true;
        }
        return false;
    }

    SkSpecialImage* get(const Key& key, SkIPoint* offset) const override {
        SkAutoMutexAcquire mutex(fMutex);
        if (Value* v = fLookup.find(key)) {
            *offset = v->fOffset;
            if (v != fLRU.head()) {
                fLRU.remove(v);
                fLRU.addToHead(v);
            }
            return v->fImage;
        }
        return nullptr;
    }

    void set(const Key& key, const SkBitmap& result, const SkIPoint& offset) override {
        SkAutoMutexAcquire mutex(fMutex);
        if (Value* v = fLookup.find(key)) {
            this->removeInternal(v);
        }
        Value* v = new Value(key, result, offset);
        fLookup.add(v);
        fLRU.addToHead(v);
        fCurrentBytes += result.getSize();
        while (fCurrentBytes > fMaxBytes) {
            Value* tail = fLRU.tail();
            SkASSERT(tail);
            if (tail == v) {
                break;
            }
            this->removeInternal(tail);
        }
    }

    void set(const Key& key, SkSpecialImage* image, const SkIPoint& offset) override {
        SkAutoMutexAcquire mutex(fMutex);
        if (Value* v = fLookup.find(key)) {
            this->removeInternal(v);
        }
        Value* v = new Value(key, image, offset);
        fLookup.add(v);
        fLRU.addToHead(v);
        fCurrentBytes += image->getSize();
        while (fCurrentBytes > fMaxBytes) {
            Value* tail = fLRU.tail();
            SkASSERT(tail);
            if (tail == v) {
                break;
            }
            this->removeInternal(tail);
        }
    }

    void purge() override {
        SkAutoMutexAcquire mutex(fMutex);
        while (fCurrentBytes > 0) {
            Value* tail = fLRU.tail();
            SkASSERT(tail);
            this->removeInternal(tail);
        }
    }

    void purgeByKeys(const Key keys[], int count) override {
        SkAutoMutexAcquire mutex(fMutex);
        for (int i = 0; i < count; i++) {
            if (Value* v = fLookup.find(keys[i])) {
                this->removeInternal(v);
            }
        }
    }

private:
    void removeInternal(Value* v) {
        if (v->fImage) {
            fCurrentBytes -= v->fImage->getSize();
        } else {
            fCurrentBytes -= v->fBitmap.getSize();
        }
        fLRU.remove(v);
        fLookup.remove(v->fKey);
        delete v;
    }
private:
    SkTDynamicHash<Value, Key>            fLookup;
    mutable SkTInternalLList<Value>       fLRU;
    size_t                                fMaxBytes;
    size_t                                fCurrentBytes;
    mutable SkMutex                       fMutex;
};

} // namespace

SkImageFilter::Cache* SkImageFilter::Cache::Create(size_t maxBytes) {
    return new CacheImpl(maxBytes);
}

SK_DECLARE_STATIC_ONCE_PTR(SkImageFilter::Cache, cache);
SkImageFilter::Cache* SkImageFilter::Cache::Get() {
    return cache.get([]{ return SkImageFilter::Cache::Create(kDefaultCacheSize); });
}

void SkImageFilter::PurgeCache() {
    Cache::Get()->purge();
}

///////////////////////////////////////////////////////////////////////////////////////////////////

SkBaseDevice* SkImageFilter::DeviceProxy::createDevice(int w, int h, TileUsage usage) {
    SkBaseDevice::CreateInfo cinfo(SkImageInfo::MakeN32Premul(w, h),
                                   kPossible_TileUsage == usage ? SkBaseDevice::kPossible_TileUsage
                                                                : SkBaseDevice::kNever_TileUsage,
                                   kUnknown_SkPixelGeometry,
                                   false,   /* preserveLCDText */
                                   true /*forImageFilter*/);
    SkBaseDevice* dev = fDevice->onCreateDevice(cinfo, nullptr);
    if (nullptr == dev) {
        const SkSurfaceProps surfaceProps(fDevice->fSurfaceProps.flags(),
                                          kUnknown_SkPixelGeometry);
        dev = SkBitmapDevice::Create(cinfo.fInfo, surfaceProps);
    }
    return dev;
}

bool SkImageFilter::DeviceProxy::filterImage(const SkImageFilter* filter, const SkBitmap& src,
                                       const SkImageFilter::Context& ctx,
                                       SkBitmap* result, SkIPoint* offset) {
    return fDevice->filterImage(filter, src, ctx, result, offset);
}

