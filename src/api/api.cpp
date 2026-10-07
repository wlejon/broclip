#include "api.h"
#include "host_clip_internal.h"
#include "broclip/clip.h"

#include <mutex>

namespace broclip::api {

namespace {

std::mutex g_services_mu;
std::shared_ptr<broclip::ClipboardManager> g_custom_clipboard_manager;
std::shared_ptr<broclip::ClipboardManager> g_default_clipboard_manager;

} // namespace

std::shared_ptr<broclip::ClipboardManager> activeClipboardManager() {
    std::lock_guard lock(g_services_mu);
    if (g_custom_clipboard_manager) return g_custom_clipboard_manager;
    if (!g_default_clipboard_manager) {
        g_default_clipboard_manager = std::make_shared<broclip::ClipboardManager>();
        g_default_clipboard_manager->start();
    }
    return g_default_clipboard_manager;
}

void setClipboardManager(std::shared_ptr<broclip::ClipboardManager> mgr) {
    {
        std::lock_guard lock(g_services_mu);
        g_custom_clipboard_manager = mgr;
    }
    connectManager(mgr ? mgr : activeClipboardManager());
}

std::shared_ptr<broclip::ClipboardManager> getClipboardManager() {
    return activeClipboardManager();
}

Value ensureBroClip() {
    ev::Persistent globalThisVal;
    auto gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        globalThisVal.set(gt.value);
    }

    ev::Persistent broP;
    auto bro = ev::globalValue("bro");
    if (bro.found && ev::isObject(bro.value)) broP.set(bro.value);
    if (!ev::isObject(broP.get()) && ev::isObject(globalThisVal.get())) {
        Value candidate = ev::getProperty(globalThisVal.get(), "bro");
        if (ev::isObject(candidate)) broP.set(candidate);
    }
    if (!ev::isObject(broP.get())) {
        broP.set(ev::createObject());
        ev::registerGlobal("bro", broP.get());
        if (ev::isObject(globalThisVal.get())) {
            globalThisVal.set(ev::setProperty(globalThisVal.get(), "bro", broP.get()));
        }
    }

    ev::Persistent clipP(ev::getProperty(broP.get(), "clip"));
    if (!ev::isObject(clipP.get())) {
        clipP.set(ev::createObject());
        broP.set(ev::setProperty(broP.get(), "clip", clipP.get()));
    }
    return clipP.get();
}

void installClip() {
    ev::Persistent clipObj(ensureBroClip());
    installClipOnto(clipObj.get());
    auto mgr = activeClipboardManager();
    connectManager(mgr);
}

void tickClipAsync() {
    drainClipEvents();
}

void shutdownClipAsync() {
    disconnectManagerSlots();
}

} // namespace broclip::api
