#pragma once

#include "render/model_inspector.hpp"

#include <QImage>
#include <QWidget>
#include <array>

class QComboBox;
class QFormLayout;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace apex::ui {
class ModelInspectCanvas;

// The Models browser's inspect area: a larger orbit/zoom/pan render of the selected model, its skin
// and body group choices, and its counts and materials. It owns the camera and choices; the panel
// feeds it the index entry and the latest render, and reads `view()` to ask the GPU host for the next.
class ModelInspectView final : public QWidget {
    Q_OBJECT
public:
    // What the browser's index knows; shown until (and without) a render.
    struct Summary {
        QString name, source;
        bool staticProp = false;
        int bodyParts = 0, skinFamilies = 0, skinsPerFamily = 0;
        std::array<float, 3> hullSize{};
    };
    explicit ModelInspectView(QWidget* parent = nullptr);
    // Selecting another model keeps the turn of the camera, and resets zoom, pan and choices.
    void setModel(const Summary& summary);
    void clearModel();
    // Null `inspection`: no preview can be made (no GPU, document or model cache).
    void setInspection(const render::ModelInspection* inspection);
    render::ModelView view() const { return view_; }
    bool hasModel() const { return !summary_.name.isEmpty(); }
    std::uint64_t shownRevision() const { return revision_; }

signals:
    void viewChanged();

private:
    void resetCamera();
    void rebuildChoices(const render::ModelInspectStats& stats);
    void updateText();
    void changeChoice();
    Summary summary_;
    render::ModelView view_;
    render::ModelInspectStats stats_;
    std::uint64_t revision_ = ~std::uint64_t{0};
    bool haveStats_ = false;
    QString status_;
    ModelInspectCanvas* canvas_ = nullptr;
    QLabel* info_ = nullptr;
    QPushButton* reset_ = nullptr;
    QFormLayout* choices_ = nullptr;
    QSpinBox* skin_ = nullptr;
    QLabel* skinLabel_ = nullptr;
    std::vector<QComboBox*> bodyCombos_;
    QListWidget* materials_ = nullptr;
    friend class ModelInspectCanvas;
};
} // namespace apex::ui
