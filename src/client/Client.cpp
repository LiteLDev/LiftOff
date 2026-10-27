#ifdef LO_CLIENT

#include "client/Client.h"

#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/locale/I18n.h"
#include "mc/locale/Localization.h"

#include "mod/I18n.h"

namespace lo::client {

ClientInstance* currentClient() { return ll::service::getClientInstance().as_ptr(); }

LocalPlayer* currentPlayer() {
    auto* c = currentClient();
    return c ? c->getLocalPlayer() : nullptr;
}

bool inLevel() {
    auto* c = currentClient();
    return c && c->getLevel() != nullptr;
}

std::string clientLocale() {
    try {
        return getI18n().getCurrentLanguage()->getFullLanguageCode();
    } catch (...) {
        return "en_US";
    }
}

std::string translateClient(std::string_view key, std::string_view fallback) {
    return std::string(translate(key, fallback, clientLocale()));
}

} // namespace lo::client

#endif // LO_CLIENT
