#include "ui/profile_store.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

namespace apex::ui {
namespace {
const QString key = QStringLiteral("compile/profiles");
bool validSpecial(int value) {
    using compile::SpecialCommand;
    for (const auto special : {SpecialCommand::ChangeDirectory, SpecialCommand::Copy, SpecialCommand::Delete,
             SpecialCommand::Rename, SpecialCommand::CopyIfExists})
        if (static_cast<int>(special) == value) return true;
    return false;
}
}

QByteArray profilesToJson(const std::vector<compile::CompileProfile>& profiles) {
    QJsonArray array;
    for (const auto& profile : profiles) {
        QJsonArray commands;
        for (const auto& command : profile.commands) {
            QJsonObject object{{"executable", QString::fromStdString(command.executable)},
                {"arguments", QString::fromStdString(command.arguments)},
                {"enabled", command.enabled}, {"wait", command.wait}};
            if (command.special) object["special"] = static_cast<int>(*command.special);
            commands.append(object);
        }
        array.append(QJsonObject{{"name", QString::fromStdString(profile.name)}, {"commands", commands},
            {"runGameAfter", profile.runGameAfter}, {"copyBspToMaps", profile.copyBspToMaps},
            {"cordonOnly", profile.cordonOnly},
            {"addonDirectory", QString::fromStdString(profile.addonDirectory)},
            {"campaignId", QString::fromStdString(profile.campaignId)},
            {"cordonMaterial", QString::fromStdString(profile.cordonMaterial)}});
    }
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

std::vector<compile::CompileProfile> profilesFromJson(const QByteArray& json) {
    std::vector<compile::CompileProfile> profiles;
    for (const auto& value : QJsonDocument::fromJson(json).array()) {
        const auto object = value.toObject();
        if (object["name"].toString().isEmpty()) continue;
        compile::CompileProfile profile;
        profile.name = object["name"].toString().toStdString();
        profile.runGameAfter = object["runGameAfter"].toBool(true);
        profile.copyBspToMaps = object["copyBspToMaps"].toBool(true);
        profile.addonDirectory = object["addonDirectory"].toString().toStdString();
        profile.campaignId = object["campaignId"].toString().toStdString();
        profile.cordonOnly = object["cordonOnly"].toBool(false);
        if (const auto material = object["cordonMaterial"].toString(); !material.isEmpty())
            profile.cordonMaterial = material.toStdString();
        for (const auto& item : object["commands"].toArray()) {
            const auto c = item.toObject();
            compile::Command command;
            command.executable = c["executable"].toString().toStdString();
            command.arguments = c["arguments"].toString().toStdString();
            command.enabled = c["enabled"].toBool(true);
            command.wait = c["wait"].toBool(true);
            if (c.contains("special") && validSpecial(c["special"].toInt(-1)))
                command.special = static_cast<compile::SpecialCommand>(c["special"].toInt());
            profile.commands.push_back(std::move(command));
        }
        profiles.push_back(std::move(profile));
    }
    return profiles;
}

std::vector<compile::CompileProfile> loadUserProfiles() {
    return profilesFromJson(QSettings().value(key).toByteArray());
}
void saveUserProfiles(const std::vector<compile::CompileProfile>& profiles) {
    QSettings().setValue(key, profilesToJson(profiles));
}

} // namespace apex::ui
