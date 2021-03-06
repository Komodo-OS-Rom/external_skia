/*
 * Copyright 2011 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkAnalyticEdge.h"
#include "SkEdge.h"
#include "SkEdgeBuilder.h"
#include "SkEdgeClipper.h"
#include "SkGeometry.h"
#include "SkLineClipper.h"
#include "SkPath.h"
#include "SkPathPriv.h"
#include "SkSafeMath.h"
#include "SkTo.h"

SkEdgeBuilder::Combine SkBasicEdgeBuilder::combineVertical(const SkEdge* edge, SkEdge* last) {
    if (last->fCurveCount || last->fDX || edge->fX != last->fX) {
        return kNo_Combine;
    }
    if (edge->fWinding == last->fWinding) {
        if (edge->fLastY + 1 == last->fFirstY) {
            last->fFirstY = edge->fFirstY;
            return kPartial_Combine;
        }
        if (edge->fFirstY == last->fLastY + 1) {
            last->fLastY = edge->fLastY;
            return kPartial_Combine;
        }
        return kNo_Combine;
    }
    if (edge->fFirstY == last->fFirstY) {
        if (edge->fLastY == last->fLastY) {
            return kTotal_Combine;
        }
        if (edge->fLastY < last->fLastY) {
            last->fFirstY = edge->fLastY + 1;
            return kPartial_Combine;
        }
        last->fFirstY = last->fLastY + 1;
        last->fLastY = edge->fLastY;
        last->fWinding = edge->fWinding;
        return kPartial_Combine;
    }
    if (edge->fLastY == last->fLastY) {
        if (edge->fFirstY > last->fFirstY) {
            last->fLastY = edge->fFirstY - 1;
            return kPartial_Combine;
        }
        last->fLastY = last->fFirstY - 1;
        last->fFirstY = edge->fFirstY;
        last->fWinding = edge->fWinding;
        return kPartial_Combine;
    }
    return kNo_Combine;
}

SkEdgeBuilder::Combine SkAnalyticEdgeBuilder::combineVertical(const SkAnalyticEdge* edge,
                                                              SkAnalyticEdge* last) {
    auto approximately_equal = [](SkFixed a, SkFixed b) {
        return SkAbs32(a - b) < 0x100;
    };

    if (last->fCurveCount || last->fDX || edge->fX != last->fX) {
        return kNo_Combine;
    }
    if (edge->fWinding == last->fWinding) {
        if (edge->fLowerY == last->fUpperY) {
            last->fUpperY = edge->fUpperY;
            last->fY = last->fUpperY;
            return kPartial_Combine;
        }
        if (approximately_equal(edge->fUpperY, last->fLowerY)) {
            last->fLowerY = edge->fLowerY;
            return kPartial_Combine;
        }
        return kNo_Combine;
    }
    if (approximately_equal(edge->fUpperY, last->fUpperY)) {
        if (approximately_equal(edge->fLowerY, last->fLowerY)) {
            return kTotal_Combine;
        }
        if (edge->fLowerY < last->fLowerY) {
            last->fUpperY = edge->fLowerY;
            last->fY = last->fUpperY;
            return kPartial_Combine;
        }
        last->fUpperY = last->fLowerY;
        last->fY = last->fUpperY;
        last->fLowerY = edge->fLowerY;
        last->fWinding = edge->fWinding;
        return kPartial_Combine;
    }
    if (approximately_equal(edge->fLowerY, last->fLowerY)) {
        if (edge->fUpperY > last->fUpperY) {
            last->fLowerY = edge->fUpperY;
            return kPartial_Combine;
        }
        last->fLowerY = last->fUpperY;
        last->fUpperY = edge->fUpperY;
        last->fY = last->fUpperY;
        last->fWinding = edge->fWinding;
        return kPartial_Combine;
    }
    return kNo_Combine;
}

template <typename Edge>
static bool is_vertical(const Edge* edge) {
    return edge->fDX         == 0
        && edge->fCurveCount == 0;
}

// TODO: we can deallocate the edge if edge->setFoo() fails
// or when we don't use it (kPartial_Combine or kTotal_Combine).

void SkBasicEdgeBuilder::addLine(const SkPoint pts[]) {
    SkEdge* edge = fAlloc.make<SkEdge>();
    if (edge->setLine(pts[0], pts[1], fClipShift)) {
        Combine combine = is_vertical(edge) && !fList.empty()
            ? this->combineVertical(edge, (SkEdge*)fList.top())
            : kNo_Combine;

        switch (combine) {
            case kTotal_Combine:    fList.pop();           break;
            case kPartial_Combine:                         break;
            case kNo_Combine:       fList.push_back(edge); break;
        }
    }
}
void SkAnalyticEdgeBuilder::addLine(const SkPoint pts[]) {
    SkAnalyticEdge* edge = fAlloc.make<SkAnalyticEdge>();
    if (edge->setLine(pts[0], pts[1])) {

        Combine combine = is_vertical(edge) && !fList.empty()
            ? this->combineVertical(edge, (SkAnalyticEdge*)fList.top())
            : kNo_Combine;

        switch (combine) {
            case kTotal_Combine:    fList.pop();           break;
            case kPartial_Combine:                         break;
            case kNo_Combine:       fList.push_back(edge); break;
        }
    }
}
void SkBezierEdgeBuilder::addLine(const SkPoint pts[]) {
    SkLine* line = fAlloc.make<SkLine>();
    if (line->set(pts)) {
        fList.push_back(line);
    }
}

void SkBasicEdgeBuilder::addQuad(const SkPoint pts[]) {
    SkQuadraticEdge* edge = fAlloc.make<SkQuadraticEdge>();
    if (edge->setQuadratic(pts, fClipShift)) {
        fList.push_back(edge);
    }
}
void SkAnalyticEdgeBuilder::addQuad(const SkPoint pts[]) {
    SkAnalyticQuadraticEdge* edge = fAlloc.make<SkAnalyticQuadraticEdge>();
    if (edge->setQuadratic(pts)) {
        fList.push_back(edge);
    }
}
void SkBezierEdgeBuilder::addQuad(const SkPoint pts[]) {
    SkQuad* quad = fAlloc.make<SkQuad>();
    if (quad->set(pts)) {
        fList.push_back(quad);
    }
}

void SkBasicEdgeBuilder::addCubic(const SkPoint pts[]) {
    SkCubicEdge* edge = fAlloc.make<SkCubicEdge>();
    if (edge->setCubic(pts, fClipShift)) {
        fList.push_back(edge);
    }
}
void SkAnalyticEdgeBuilder::addCubic(const SkPoint pts[]) {
    SkAnalyticCubicEdge* edge = fAlloc.make<SkAnalyticCubicEdge>();
    if (edge->setCubic(pts)) {
        fList.push_back(edge);
    }
}
void SkBezierEdgeBuilder::addCubic(const SkPoint pts[]) {
    SkCubic* cubic = fAlloc.make<SkCubic>();
    if (cubic->set(pts)) {
        fList.push_back(cubic);
    }
}

// TODO: merge addLine() and addPolyLine()?

SkEdgeBuilder::Combine SkBasicEdgeBuilder::addPolyLine(SkPoint pts[],
                                                       char* arg_edge, char** arg_edgePtr) {
    auto edge    = (SkEdge*) arg_edge;
    auto edgePtr = (SkEdge**)arg_edgePtr;

    if (edge->setLine(pts[0], pts[1], fClipShift)) {
        return is_vertical(edge) && edgePtr > (SkEdge**)fEdgeList
            ? this->combineVertical(edge, edgePtr[-1])
            : kNo_Combine;
    }
    return SkEdgeBuilder::kPartial_Combine;  // A convenient lie.  Same do-nothing behavior.
}
SkEdgeBuilder::Combine SkAnalyticEdgeBuilder::addPolyLine(SkPoint pts[],
                                                          char* arg_edge, char** arg_edgePtr) {
    auto edge    = (SkAnalyticEdge*) arg_edge;
    auto edgePtr = (SkAnalyticEdge**)arg_edgePtr;

    if (edge->setLine(pts[0], pts[1])) {
        return is_vertical(edge) && edgePtr > (SkAnalyticEdge**)fEdgeList
            ? this->combineVertical(edge, edgePtr[-1])
            : kNo_Combine;
    }
    return SkEdgeBuilder::kPartial_Combine;  // As above.
}
SkEdgeBuilder::Combine SkBezierEdgeBuilder::addPolyLine(SkPoint pts[],
                                                        char* arg_edge, char** arg_edgePtr) {
    auto edge = (SkLine*)arg_edge;

    if (edge->set(pts)) {
        return kNo_Combine;
    }
    return SkEdgeBuilder::kPartial_Combine;  // As above.
}

SkRect SkBasicEdgeBuilder::recoverClip(const SkIRect& src) const {
    return { SkIntToScalar(src.fLeft   >> fClipShift),
             SkIntToScalar(src.fTop    >> fClipShift),
             SkIntToScalar(src.fRight  >> fClipShift),
             SkIntToScalar(src.fBottom >> fClipShift), };
}
SkRect SkAnalyticEdgeBuilder::recoverClip(const SkIRect& src) const {
    return SkRect::Make(src);
}
SkRect SkBezierEdgeBuilder::recoverClip(const SkIRect& src) const {
    return SkRect::Make(src);
}

char* SkBasicEdgeBuilder::allocEdges(size_t n, size_t* size) {
    *size = sizeof(SkEdge);
    return (char*)fAlloc.makeArrayDefault<SkEdge>(n);
}
char* SkAnalyticEdgeBuilder::allocEdges(size_t n, size_t* size) {
    *size = sizeof(SkAnalyticEdge);
    return (char*)fAlloc.makeArrayDefault<SkAnalyticEdge>(n);
}
char* SkBezierEdgeBuilder::allocEdges(size_t n, size_t* size) {
    *size = sizeof(SkLine);
    return (char*)fAlloc.makeArrayDefault<SkLine>(n);
}

// TODO: maybe get rid of buildPoly() entirely?
int SkEdgeBuilder::buildPoly(const SkPath& path, const SkIRect* iclip, bool canCullToTheRight) {
    SkPath::Iter    iter(path, true);
    SkPoint         pts[4];
    SkPath::Verb    verb;

    size_t maxEdgeCount = path.countPoints();
    if (iclip) {
        // clipping can turn 1 line into (up to) kMaxClippedLineSegments, since
        // we turn portions that are clipped out on the left/right into vertical
        // segments.
        SkSafeMath safe;
        maxEdgeCount = safe.mul(maxEdgeCount, SkLineClipper::kMaxClippedLineSegments);
        if (!safe) {
            return 0;
        }
    }

    size_t edgeSize;
    char* edge = this->allocEdges(maxEdgeCount, &edgeSize);

    SkDEBUGCODE(char* edgeStart = edge);
    char** edgePtr = fAlloc.makeArrayDefault<char*>(maxEdgeCount);
    fEdgeList = (void**)edgePtr;

    if (iclip) {
        SkRect clip = this->recoverClip(*iclip);

        while ((verb = iter.next(pts, false)) != SkPath::kDone_Verb) {
            switch (verb) {
                case SkPath::kMove_Verb:
                case SkPath::kClose_Verb:
                    // we ignore these, and just get the whole segment from
                    // the corresponding line/quad/cubic verbs
                    break;
                case SkPath::kLine_Verb: {
                    SkPoint lines[SkLineClipper::kMaxPoints];
                    int lineCount = SkLineClipper::ClipLine(pts, clip, lines, canCullToTheRight);
                    SkASSERT(lineCount <= SkLineClipper::kMaxClippedLineSegments);
                    for (int i = 0; i < lineCount; i++) {
                        switch( this->addPolyLine(lines + i, edge, edgePtr) ) {
                            case kTotal_Combine:   edgePtr--; break;
                            case kPartial_Combine:            break;
                            case kNo_Combine: *edgePtr++ = edge;
                                               edge += edgeSize;
                        }
                    }
                    break;
                }
                default:
                    SkDEBUGFAIL("unexpected verb");
                    break;
            }
        }
    } else {
        while ((verb = iter.next(pts, false)) != SkPath::kDone_Verb) {
            switch (verb) {
                case SkPath::kMove_Verb:
                case SkPath::kClose_Verb:
                    // we ignore these, and just get the whole segment from
                    // the corresponding line/quad/cubic verbs
                    break;
                case SkPath::kLine_Verb: {
                    switch( this->addPolyLine(pts, edge, edgePtr) ) {
                        case kTotal_Combine:   edgePtr--; break;
                        case kPartial_Combine:            break;
                        case kNo_Combine: *edgePtr++ = edge;
                                           edge += edgeSize;
                    }
                    break;
                }
                default:
                    SkDEBUGFAIL("unexpected verb");
                    break;
            }
        }
    }
    SkASSERT((size_t)(edge - edgeStart) <= maxEdgeCount * edgeSize);
    SkASSERT((size_t)(edgePtr - (char**)fEdgeList) <= maxEdgeCount);
    return SkToInt(edgePtr - (char**)fEdgeList);
}

int SkEdgeBuilder::build(const SkPath& path, const SkIRect* iclip, bool canCullToTheRight) {
    SkAutoConicToQuads quadder;
    const SkScalar conicTol = SK_Scalar1 / 4;

    SkPath::Iter    iter(path, true);
    SkPoint         pts[4];
    SkPath::Verb    verb;

    bool is_finite = true;

    if (iclip) {
        SkRect clip = this->recoverClip(*iclip);
        SkEdgeClipper clipper(canCullToTheRight);

        auto apply_clipper = [this, &clipper, &is_finite] {
            SkPoint      pts[4];
            SkPath::Verb verb;

            while ((verb = clipper.next(pts)) != SkPath::kDone_Verb) {
                const int count = SkPathPriv::PtsInIter(verb);
                if (!SkScalarsAreFinite(&pts[0].fX, count*2)) {
                    is_finite = false;
                    return;
                }
                switch (verb) {
                    case SkPath::kLine_Verb:  this->addLine (pts); break;
                    case SkPath::kQuad_Verb:  this->addQuad (pts); break;
                    case SkPath::kCubic_Verb: this->addCubic(pts); break;
                    default: break;
                }
            }
        };

        while ((verb = iter.next(pts, false)) != SkPath::kDone_Verb) {
            switch (verb) {
                case SkPath::kMove_Verb:
                case SkPath::kClose_Verb:
                    // we ignore these, and just get the whole segment from
                    // the corresponding line/quad/cubic verbs
                    break;
                case SkPath::kLine_Verb:
                    if (clipper.clipLine(pts[0], pts[1], clip)) {
                        apply_clipper();
                    }
                    break;
                case SkPath::kQuad_Verb:
                    if (clipper.clipQuad(pts, clip)) {
                        apply_clipper();
                    }
                    break;
                case SkPath::kConic_Verb: {
                    const SkPoint* quadPts = quadder.computeQuads(
                                          pts, iter.conicWeight(), conicTol);
                    for (int i = 0; i < quadder.countQuads(); ++i) {
                        if (clipper.clipQuad(quadPts, clip)) {
                            apply_clipper();
                        }
                        quadPts += 2;
                    }
                } break;
                case SkPath::kCubic_Verb:
                    if (clipper.clipCubic(pts, clip)) {
                        apply_clipper();
                    }
                    break;
                default:
                    SkDEBUGFAIL("unexpected verb");
                    break;
            }
        }
    } else {
        while ((verb = iter.next(pts, false)) != SkPath::kDone_Verb) {
            auto handle_quad = [this](const SkPoint pts[3]) {
                SkPoint monoX[5];
                int n = SkChopQuadAtYExtrema(pts, monoX);
                for (int i = 0; i <= n; i++) {
                    this->addQuad(&monoX[i * 2]);
                }
            };

            switch (verb) {
                case SkPath::kMove_Verb:
                case SkPath::kClose_Verb:
                    // we ignore these, and just get the whole segment from
                    // the corresponding line/quad/cubic verbs
                    break;
                case SkPath::kLine_Verb:
                    this->addLine(pts);
                    break;
                case SkPath::kQuad_Verb: {
                    handle_quad(pts);
                    break;
                }
                case SkPath::kConic_Verb: {
                    const SkPoint* quadPts = quadder.computeQuads(
                                          pts, iter.conicWeight(), conicTol);
                    for (int i = 0; i < quadder.countQuads(); ++i) {
                        handle_quad(quadPts);
                        quadPts += 2;
                    }
                } break;
                case SkPath::kCubic_Verb: {
                    if (!this->chopCubics()) {
                        this->addCubic(pts);
                        break;
                    }
                    SkPoint monoY[10];
                    int n = SkChopCubicAtYExtrema(pts, monoY);
                    for (int i = 0; i <= n; i++) {
                        this->addCubic(&monoY[i * 3]);
                    }
                    break;
                }
                default:
                    SkDEBUGFAIL("unexpected verb");
                    break;
            }
        }
    }
    fEdgeList = fList.begin();
    return is_finite ? fList.count() : 0;
}

int SkEdgeBuilder::buildEdges(const SkPath& path,
                              const SkIRect* shiftedClip) {
    // If we're convex, then we need both edges, even if the right edge is past the clip.
    const bool canCullToTheRight = !path.isConvex();

    // We can use our buildPoly() optimization if all the segments are lines.
    // (Edges are homogenous and stored contiguously in memory, no need for indirection.)
    const int count = SkPath::kLine_SegmentMask == path.getSegmentMasks()
        ? this->buildPoly(path, shiftedClip, canCullToTheRight)
        : this->build    (path, shiftedClip, canCullToTheRight);

    SkASSERT(count >= 0);

    // If we can't cull to the right, we should have count > 1 (or 0),
    // unless we're in DAA which doesn't need to chop edges at y extrema.
    // For example, a single cubic edge with a valley shape \_/ is fine for DAA.
    if (!canCullToTheRight && count == 1) {
        SkASSERT(!this->chopCubics());
    }

    return count;
}
