/*
 * Copyright 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#undef LOG_TAG
#define LOG_TAG "SurfaceFlinger"

#include <android-base/logging.h>

#include "LayerHierarchy.h"
#include "LayerLog.h"
#include "SwapErase.h"

namespace android::surfaceflinger::frontend {

namespace {
auto layerZCompare = [](const std::pair<LayerHierarchy*, LayerHierarchy::Variant>& lhs,
                        const std::pair<LayerHierarchy*, LayerHierarchy::Variant>& rhs) {
    auto lhsLayer = lhs.first->getLayer();
    auto rhsLayer = rhs.first->getLayer();
    if (lhsLayer->layerStack.id != rhsLayer->layerStack.id) {
        return lhsLayer->layerStack.id < rhsLayer->layerStack.id;
    }
    if (lhsLayer->z != rhsLayer->z) {
        return lhsLayer->z < rhsLayer->z;
    }
    return lhsLayer->id < rhsLayer->id;
};

void insertSorted(std::vector<std::pair<LayerHierarchy*, LayerHierarchy::Variant>>& vec,
                  std::pair<LayerHierarchy*, LayerHierarchy::Variant> value) {
    auto it = std::upper_bound(vec.begin(), vec.end(), value, layerZCompare);
    vec.insert(it, std::move(value));
}
} // namespace

LayerHierarchy::LayerHierarchy(RequestedLayerState* layer) : mLayer(layer) {}

LayerHierarchy::LayerHierarchy(const LayerHierarchy& hierarchy, bool childrenOnly) {
    mLayer = (childrenOnly) ? nullptr : hierarchy.mLayer;
    mChildren = hierarchy.mChildren;
}

void LayerHierarchy::traverse(const Visitor& visitor,
                              const LayerHierarchy::TraversalPath& traversalPath,
                              uint32_t depth) const {
    LLOG_ALWAYS_FATAL_WITH_TRACE_IF(depth > 50,
                                    "Cycle detected in LayerHierarchy::traverse. See "
                                    "traverse_stack_overflow_transactions.winscope");

    if (mLayer) {
        bool breakTraversal = !visitor(*this, traversalPath);
        if (breakTraversal) {
            return;
        }
    }

    LLOG_ALWAYS_FATAL_WITH_TRACE_IF(traversalPath.hasRelZLoop(), "Found relative z loop layerId:%d",
                                    traversalPath.invalidRelativeRootId);
    for (auto& [child, childVariant] : mChildren) {
        child->traverse(visitor, traversalPath.makeChild(child->mLayer->id, childVariant),
                        depth + 1);
    }
}

void LayerHierarchy::traverseInZOrder(const Visitor& visitor,
                                      const LayerHierarchy::TraversalPath& traversalPath) const {
    bool traverseThisLayer = (mLayer != nullptr);
    for (auto it = mChildren.begin(); it < mChildren.end(); it++) {
        auto& [child, childVariant] = *it;
        if (traverseThisLayer && child->getLayer()->z >= 0) {
            traverseThisLayer = false;
            bool breakTraversal = !visitor(*this, traversalPath);
            if (breakTraversal) {
                return;
            }
        }
        if (childVariant == LayerHierarchy::Variant::Detached) {
            continue;
        }
        child->traverseInZOrder(visitor, traversalPath.makeChild(child->mLayer->id, childVariant));
    }

    if (traverseThisLayer) {
        visitor(*this, traversalPath);
    }
}

void LayerHierarchy::addChild(LayerHierarchy* child, LayerHierarchy::Variant variant) {
    insertSorted(mChildren, {child, variant});
}

void LayerHierarchy::removeChild(LayerHierarchy* child) {
    auto it = std::find_if(mChildren.begin(), mChildren.end(),
                           [child](const std::pair<LayerHierarchy*, Variant>& x) {
                               return x.first == child;
                           });
    LLOG_ALWAYS_FATAL_WITH_TRACE_IF(it == mChildren.end(), "Could not find child!");
    mChildren.erase(it);
}

void LayerHierarchy::sortChildrenByZOrder() {
    std::sort(mChildren.begin(), mChildren.end(), layerZCompare);
}

void LayerHierarchy::updateChild(LayerHierarchy* hierarchy, LayerHierarchy::Variant variant) {
    auto it = std::find_if(mChildren.begin(), mChildren.end(),
                           [hierarchy](std::pair<LayerHierarchy*, Variant>& child) {
                               return child.first == hierarchy;
                           });
    LLOG_ALWAYS_FATAL_WITH_TRACE_IF(it == mChildren.end(), "Could not find child!");
    it->second = variant;
}

const RequestedLayerState* LayerHierarchy::getLayer() const {
    return mLayer;
}

const LayerHierarchy* LayerHierarchy::getRelativeParent() const {
    return mRelativeParent;
}

const LayerHierarchy* LayerHierarchy::getParent() const {
    return mParent;
}

std::string LayerHierarchy::getDebugStringShort() const {
    std::string debug = "LayerHierarchy{";
    debug += ((mLayer) ? mLayer->getDebugString() : "root") + " ";
    if (mChildren.empty()) {
        debug += "no children";
    } else {
        debug += std::to_string(mChildren.size()) + " children";
    }
    return debug + "}";
}

void LayerHierarchy::dump(std::ostream& out, const std::string& prefix,
                          LayerHierarchy::Variant variant, bool isLastChild,
                          bool includeMirroredHierarchy) const {
    if (!mLayer) {
        out << " ROOT";
    } else {
        out << prefix + (isLastChild ? "└─ " : "├─ ");
        if (variant == LayerHierarchy::Variant::Relative) {
            out << "(Relative) ";
        } else if (LayerHierarchy::isMirror(variant)) {
            if (!includeMirroredHierarchy) {
                out << "(Mirroring) " << *mLayer << "\n" + prefix + "   └─ ...";
                return;
            }
            out << "(Mirroring) ";
        }

        out << *mLayer << " pid=" << mLayer->ownerPid.val() << " uid=" << mLayer->ownerUid.val();
    }

    for (size_t i = 0; i < mChildren.size(); i++) {
        auto& [child, childVariant] = mChildren[i];
        if (childVariant == LayerHierarchy::Variant::Detached) continue;
        const bool lastChild = i == (mChildren.size() - 1);
        std::string childPrefix = prefix;
        if (mLayer) {
            childPrefix += (isLastChild ? "   " : "│  ");
        }
        out << "\n";
        child->dump(out, childPrefix, childVariant, lastChild, includeMirroredHierarchy);
    }
    return;
}

bool LayerHierarchy::hasRelZLoop(uint32_t& outInvalidRelativeRoot) const {
    outInvalidRelativeRoot = UNASSIGNED_LAYER_ID;
    traverse([&outInvalidRelativeRoot](const LayerHierarchy&,
                                       const LayerHierarchy::TraversalPath& traversalPath) -> bool {
        if (traversalPath.hasRelZLoop()) {
            outInvalidRelativeRoot = traversalPath.invalidRelativeRootId;
            return false;
        }
        return true;
    });
    return outInvalidRelativeRoot != UNASSIGNED_LAYER_ID;
}

void LayerHierarchyBuilder::init(const std::vector<std::unique_ptr<RequestedLayerState>>& layers) {
    mLayerIdToHierarchy.clear();
    mHierarchies.clear();
    mRoot = nullptr;
    mOffscreenRoot = nullptr;

    mHierarchies.reserve(layers.size());
    mLayerIdToHierarchy.reserve(layers.size());
    for (auto& layer : layers) {
        mHierarchies.emplace_back(std::make_unique<LayerHierarchy>(layer.get()));
        mLayerIdToHierarchy[layer->id] = mHierarchies.back().get();
    }
    for (const auto& layer : layers) {
        onLayerAdded(layer.get());
    }
    detachHierarchyFromRelativeParent(&mOffscreenRoot);
    mInitialized = true;
}

void LayerHierarchyBuilder::attachToParent(LayerHierarchy* hierarchy) {
    auto layer = hierarchy->mLayer;
    LayerHierarchy::Variant type = layer->hasValidRelativeParent()
            ? LayerHierarchy::Variant::Detached
            : LayerHierarchy::Variant::Attached;

    LayerHierarchy* parent;

    if (layer->parentId != UNASSIGNED_LAYER_ID) {
        parent = getHierarchyFromId(layer->parentId);
    } else if (layer->canBeRoot) {
        parent = &mRoot;
    } else {
        parent = &mOffscreenRoot;
    }
    parent->addChild(hierarchy, type);
    hierarchy->mParent = parent;
}

void LayerHierarchyBuilder::detachFromParent(LayerHierarchy* hierarchy) {
    hierarchy->mParent->removeChild(hierarchy);
    hierarchy->mParent = nullptr;
}

void LayerHierarchyBuilder::attachToRelativeParent(LayerHierarchy* hierarchy) {
    auto layer = hierarchy->mLayer;
    if (!layer->hasValidRelativeParent() || hierarchy->mRelativeParent) {
        return;
    }

    if (layer->relativeParentId != UNASSIGNED_LAYER_ID) {
        hierarchy->mRelativeParent = getHierarchyFromId(layer->relativeParentId);
    } else {
        hierarchy->mRelativeParent = &mOffscreenRoot;
    }
    hierarchy->mRelativeParent->addChild(hierarchy, LayerHierarchy::Variant::Relative);
    hierarchy->mParent->updateChild(hierarchy, LayerHierarchy::Variant::Detached);
}

void LayerHierarchyBuilder::detachFromRelativeParent(LayerHierarchy* hierarchy) {
    if (hierarchy->mRelativeParent) {
        hierarchy->mRelativeParent->removeChild(hierarchy);
    }
    hierarchy->mRelativeParent = nullptr;
    hierarchy->mParent->updateChild(hierarchy, LayerHierarchy::Variant::Attached);
}

std::vector<LayerHierarchy*> LayerHierarchyBuilder::getDescendants(LayerHierarchy* root) {
    std::vector<LayerHierarchy*> hierarchies;
    hierarchies.push_back(root);
    std::vector<LayerHierarchy*> descendants;
    for (size_t i = 0; i < hierarchies.size(); i++) {
        LayerHierarchy* hierarchy = hierarchies[i];
        if (hierarchy->mLayer) {
            descendants.push_back(hierarchy);
        }
        for (auto& [child, childVariant] : hierarchy->mChildren) {
            if (childVariant == LayerHierarchy::Variant::Detached ||
                childVariant == LayerHierarchy::Variant::Attached) {
                hierarchies.push_back(child);
            }
        }
    }
    return descendants;
}

void LayerHierarchyBuilder::attachHierarchyToRelativeParent(LayerHierarchy* root) {
    std::vector<LayerHierarchy*> hierarchiesToAttach = getDescendants(root);
    for (LayerHierarchy* hierarchy : hierarchiesToAttach) {
        attachToRelativeParent(hierarchy);
    }
}

void LayerHierarchyBuilder::detachHierarchyFromRelativeParent(LayerHierarchy* root) {
    std::vector<LayerHierarchy*> hierarchiesToDetach = getDescendants(root);
    for (LayerHierarchy* hierarchy : hierarchiesToDetach) {
        detachFromRelativeParent(hierarchy);
    }
}

void LayerHierarchyBuilder::onLayerAdded(RequestedLayerState* layer) {
    LayerHierarchy* hierarchy = getHierarchyFromId(layer->id);
    attachToParent(hierarchy);
    attachToRelativeParent(hierarchy);

    for (uint32_t mirrorId : layer->mirrorIds) {
        LayerHierarchy* mirror = getHierarchyFromId(mirrorId);
        hierarchy->addChild(mirror, LayerHierarchy::Variant::Mirror);
    }
    if (FlagManager::getInstance().detached_mirror()) {
        if (layer->layerIdToMirror != UNASSIGNED_LAYER_ID) {
            LayerHierarchy* mirror = getHierarchyFromId(layer->layerIdToMirror);
            hierarchy->addChild(mirror, LayerHierarchy::Variant::Detached_Mirror);
        }
    }
}

void LayerHierarchyBuilder::onLayerDestroyed(RequestedLayerState* layer) {
    LLOGV(layer->id, "");
    LayerHierarchy* hierarchy = getHierarchyFromId(layer->id, /*crashOnFailure=*/false);
    if (!hierarchy) {
        // Layer was never part of the hierarchy if it was created and destroyed in the same
        // transaction.
        return;
    }
    // detach from parent
    detachFromRelativeParent(hierarchy);
    detachFromParent(hierarchy);

    // detach children
    for (auto& [child, variant] : hierarchy->mChildren) {
        if (variant == LayerHierarchy::Variant::Attached ||
            variant == LayerHierarchy::Variant::Detached) {
            mOffscreenRoot.addChild(child, LayerHierarchy::Variant::Attached);
            child->mParent = &mOffscreenRoot;
        } else if (variant == LayerHierarchy::Variant::Relative) {
            mOffscreenRoot.addChild(child, LayerHierarchy::Variant::Attached);
            child->mRelativeParent = &mOffscreenRoot;
        }
    }

    swapErase(mHierarchies, [hierarchy](std::unique_ptr<LayerHierarchy>& layerHierarchy) {
        return layerHierarchy.get() == hierarchy;
    });
    mLayerIdToHierarchy.erase(layer->id);
}

void LayerHierarchyBuilder::updateMirrorLayer(RequestedLayerState* layer) {
    LayerHierarchy* hierarchy = getHierarchyFromId(layer->id);
    auto it = hierarchy->mChildren.begin();
    while (it != hierarchy->mChildren.end()) {
        if (LayerHierarchy::isMirror(it->second)) {
            it = hierarchy->mChildren.erase(it);
        } else {
            it++;
        }
    }

    for (uint32_t mirrorId : layer->mirrorIds) {
        hierarchy->addChild(getHierarchyFromId(mirrorId), LayerHierarchy::Variant::Mirror);
    }
    if (FlagManager::getInstance().detached_mirror()) {
        if (layer->layerIdToMirror != UNASSIGNED_LAYER_ID) {
            hierarchy->addChild(getHierarchyFromId(layer->layerIdToMirror),
                                LayerHierarchy::Variant::Detached_Mirror);
        }
    }
}

void LayerHierarchyBuilder::doUpdate(
        const std::vector<std::unique_ptr<RequestedLayerState>>& layers,
        const std::vector<std::unique_ptr<RequestedLayerState>>& destroyedLayers) {
    // rebuild map
    for (auto& layer : layers) {
        if (layer->changes.test(RequestedLayerState::Changes::Created)) {
            mHierarchies.emplace_back(std::make_unique<LayerHierarchy>(layer.get()));
            mLayerIdToHierarchy[layer->id] = mHierarchies.back().get();
        }
    }

    for (auto& layer : layers) {
        if (layer->changes.get() == 0) {
            continue;
        }
        if (layer->changes.test(RequestedLayerState::Changes::Created)) {
            onLayerAdded(layer.get());
            continue;
        }
        LayerHierarchy* hierarchy = getHierarchyFromId(layer->id);
        if (layer->changes.test(RequestedLayerState::Changes::Parent)) {
            detachFromParent(hierarchy);
            attachToParent(hierarchy);
        }
        if (layer->changes.test(RequestedLayerState::Changes::RelativeParent)) {
            detachFromRelativeParent(hierarchy);
            attachToRelativeParent(hierarchy);
        }
        if (layer->changes.test(RequestedLayerState::Changes::Z)) {
            hierarchy->mParent->sortChildrenByZOrder();
            if (hierarchy->mRelativeParent) {
                hierarchy->mRelativeParent->sortChildrenByZOrder();
            }
        }
        if (layer->changes.test(RequestedLayerState::Changes::Mirror)) {
            updateMirrorLayer(layer.get());
        }
    }

    for (auto& layer : destroyedLayers) {
        onLayerDestroyed(layer.get());
    }
    // When moving from onscreen to offscreen and vice versa, we need to attach and detach
    // from our relative parents. This walks down both trees to do so. We can optimize this
    // further by tracking onscreen, offscreen state in LayerHierarchy.
    detachHierarchyFromRelativeParent(&mOffscreenRoot);
    attachHierarchyToRelativeParent(&mRoot);
}

void LayerHierarchyBuilder::update(LayerLifecycleManager& layerLifecycleManager) {
    if (!mInitialized) {
        SFTRACE_NAME("LayerHierarchyBuilder:init");
        init(layerLifecycleManager.getLayers());
    } else if (layerLifecycleManager.getGlobalChanges().test(
                       RequestedLayerState::Changes::Hierarchy)) {
        SFTRACE_NAME("LayerHierarchyBuilder:update");
        doUpdate(layerLifecycleManager.getLayers(), layerLifecycleManager.getDestroyedLayers());
    } else {
        return; // nothing to do
    }

    uint32_t invalidRelativeRoot;
    bool hasRelZLoop = mRoot.hasRelZLoop(invalidRelativeRoot);
    while (hasRelZLoop) {
        SFTRACE_NAME("FixRelZLoop");
        TransactionTraceWriter::getInstance().invoke("relz_loop_detected",
                                                     /*overwrite=*/false);
        layerLifecycleManager.fixRelativeZLoop(invalidRelativeRoot);
        // reinitialize the hierarchy with the updated layer data
        init(layerLifecycleManager.getLayers());
        // check if we have any remaining loops
        hasRelZLoop = mRoot.hasRelZLoop(invalidRelativeRoot);
    }
}

const LayerHierarchy& LayerHierarchyBuilder::getHierarchy() const {
    return mRoot;
}

const LayerHierarchy& LayerHierarchyBuilder::getOffscreenHierarchy() const {
    return mOffscreenRoot;
}

std::string LayerHierarchyBuilder::getDebugString(uint32_t layerId, uint32_t depth) const {
    if (depth > 10) return "too deep, loop?";
    if (layerId == UNASSIGNED_LAYER_ID) return "";
    auto it = mLayerIdToHierarchy.find(layerId);
    if (it == mLayerIdToHierarchy.end()) return "not found";

    LayerHierarchy* hierarchy = it->second;
    if (!hierarchy->mLayer) return "none";

    std::string debug =
            "[" + std::to_string(hierarchy->mLayer->id) + "] " + hierarchy->mLayer->name;
    if (hierarchy->mRelativeParent) {
        debug += " Relative:" + hierarchy->mRelativeParent->getDebugStringShort();
    }
    if (hierarchy->mParent) {
        debug += " Parent:" + hierarchy->mParent->getDebugStringShort();
    }
    return debug;
}

LayerHierarchy LayerHierarchyBuilder::getPartialHierarchy(uint32_t layerId,
                                                          bool childrenOnly) const {
    auto it = mLayerIdToHierarchy.find(layerId);
    if (it == mLayerIdToHierarchy.end()) return {nullptr};

    LayerHierarchy hierarchy(*it->second, childrenOnly);
    return hierarchy;
}

LayerHierarchy* LayerHierarchyBuilder::getHierarchyFromId(uint32_t layerId, bool crashOnFailure) {
    auto it = mLayerIdToHierarchy.find(layerId);
    if (it == mLayerIdToHierarchy.end()) {
        LLOG_ALWAYS_FATAL_WITH_TRACE_IF(crashOnFailure, "Could not find hierarchy for layer id %d",
                                        layerId);
        return nullptr;
    };

    return it->second;
}

void LayerHierarchyBuilder::logSampledChildren(const LayerHierarchy& hierarchy) const {
    LOG(ERROR) << "Dumping random sampling of child layers.";
    int sampleSize = static_cast<int>(hierarchy.mChildren.size() / 100 + 1);
    for (const auto& [child, variant] : hierarchy.mChildren) {
        if (rand() % sampleSize == 0) {
            LOG(ERROR) << "Child Layer: " << *(child->mLayer);
        }
    }
}

void LayerHierarchyBuilder::dumpLayerSample(const LayerHierarchy& root) const {
    LOG(ERROR) << "Dumping layer keeping > 20 children alive:";
    // If mLayer is nullptr, it will be skipped while traversing.
    if (!root.mLayer && root.mChildren.size() > 20) {
        LOG(ERROR) << "ROOT has " << root.mChildren.size() << " children";
        logSampledChildren(root);
    }
    root.traverse([&](const LayerHierarchy& hierarchy, const auto&) -> bool {
        if (hierarchy.mChildren.size() <= 20) {
            return true;
        }
        // mLayer is ensured to be non-null. See LayerHierarchy::traverse.
        const auto* layer = hierarchy.mLayer;
        const auto childrenCount = hierarchy.mChildren.size();
        LOG(ERROR) << "Layer " << *layer << " has " << childrenCount << " children";

        const auto* parent = hierarchy.mParent;
        while (parent != nullptr) {
            if (!parent->mLayer) break;
            LOG(ERROR) << "Parent Layer: " << *(parent->mLayer);
            parent = parent->mParent;
        }

        logSampledChildren(hierarchy);
        // Stop traversing.
        return false;
    });
    LOG(ERROR) << "Dumping random sampled layers.";
    size_t numLayers = 0;
    root.traverse([&](const LayerHierarchy& hierarchy, const auto&) -> bool {
        if (hierarchy.mLayer) numLayers++;
        if ((rand() % 20 == 13) && hierarchy.mLayer) {
            LOG(ERROR) << "Layer: " << *(hierarchy.mLayer);
        }
        return true;
    });
    LOG(ERROR) << "Total layer count: " << numLayers;
}

const LayerHierarchy::TraversalPath LayerHierarchy::TraversalPath::ROOT =
        {.id = UNASSIGNED_LAYER_ID, .variant = LayerHierarchy::Attached};

std::string LayerHierarchy::TraversalPath::toString() const {
    if (id == UNASSIGNED_LAYER_ID) {
        return "TraversalPath{ROOT}";
    }
    std::stringstream ss;
    ss << "TraversalPath{.id = " << id;

    if (!mirrorRootIds.empty()) {
        ss << ", .mirrorRootIds=";
        for (auto rootId : mirrorRootIds) {
            ss << rootId << ",";
        }
    }

    if (!relativeRootIds.empty()) {
        ss << ", .relativeRootIds=";
        for (auto rootId : relativeRootIds) {
            ss << rootId << ",";
        }
    }

    if (hasRelZLoop()) {
        ss << "hasRelZLoop=true invalidRelativeRootId=" << invalidRelativeRootId << ",";
    }
    ss << "}";
    return ss.str();
}

LayerHierarchy::TraversalPath LayerHierarchy::TraversalPath::makeChild(
        uint32_t layerId, LayerHierarchy::Variant variant) const {
    TraversalPath child{*this};
    child.id = layerId;
    child.variant = variant;
    if (LayerHierarchy::isMirror(variant)) {
        child.mirrorRootIds.emplace_back(id);
    } else if (variant == LayerHierarchy::Variant::Relative) {
        if (std::find(relativeRootIds.begin(), relativeRootIds.end(), layerId) !=
            relativeRootIds.end()) {
            child.invalidRelativeRootId = layerId;
        }
        child.relativeRootIds.emplace_back(layerId);
    } else if (variant == LayerHierarchy::Variant::Detached) {
        child.detached = true;
    }
    return child;
}

} // namespace android::surfaceflinger::frontend
