#ifndef MODELANIMATOR_H
#define MODELANIMATOR_H

#include "ModelDocument.h"

#include <vector>

namespace ModelAnim {

// Value of a channel at `time` (seconds) into out[0..2] (T/S) or out[0..3]
// (rotation x y z w, normalised). `hint` caches the last key so playback
// finds the next one in O(1); any value is valid. Before the first key the
// first value holds, after the last the last.
void sample(const ModelChannel &channel, float time, quint32 &hint, float *out);

} // namespace ModelAnim

// The animated state of a document's nodes: local TRS per node, world
// matrices and the skin joint matrices the renderer uploads.
// Cost per frame: O(channels) samples (O(1) each during playback) plus one
// matrix product per node and per joint.
class ModelPose
{
public:
    // Rest pose; drops key hints.
    void reset(const ModelDocument &doc);
    // Overwrites the channels' targets with their values at `time`; nodes
    // the animation does not touch keep their rest values.
    void apply(const ModelAnimation &animation, float time);
    void updateWorld(const ModelDocument &doc, const QMatrix4x4 &root);

    const std::vector<QMatrix4x4> &world() const { return m_world; }

    // Joint matrices (joint world x inverse bind) of every skin, column-major
    // 16 floats each, skins concatenated; skinBase[s] is skin s's first joint.
    void jointMatrices(const ModelDocument &doc, const QMatrix4x4 &root, std::vector<float> &out,
                       std::vector<int> &skinBase) const;

private:
    std::vector<QVector3D> m_translation;
    std::vector<QQuaternion> m_rotation;
    std::vector<QVector3D> m_scale;
    std::vector<QMatrix4x4> m_world;
    std::vector<quint32> m_hints;
    const ModelAnimation *m_hintsFor = nullptr;
};

#endif // MODELANIMATOR_H
