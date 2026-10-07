#pragma once

#include "broclip/clip.h"

#include <memory>

namespace broclip::api {

/// Mounts `bro.clip` in the current Bronze realm.
void installClip();

/// Pumps async clip events and dispatches JS event listeners.
void tickClipAsync();

/// Disconnects manager listener slots and shuts down async clip handling.
void shutdownClipAsync();

/// Sets the clipboard manager used by the API.
void setClipboardManager(std::shared_ptr<broclip::ClipboardManager> mgr);

/// Gets the clipboard manager currently used by the API.
std::shared_ptr<broclip::ClipboardManager> getClipboardManager();

} // namespace broclip::api

using broclip::api::installClip;
using broclip::api::tickClipAsync;
using broclip::api::shutdownClipAsync;
using broclip::api::setClipboardManager;
using broclip::api::getClipboardManager;
