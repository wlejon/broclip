#pragma once

#include "object_builder.h"
#include "broclip/clip.h"
#include "broclip/types.h"

#include <memory>
#include <string>
#include <string_view>

namespace broclip::api {

Value ensureBroClip();
void installClipOnto(Value clipVal);
void drainClipEvents();
void disconnectManagerSlots();
void clearClipSubscriptions();

std::shared_ptr<broclip::ClipboardManager> activeClipboardManager();
void connectManager(const std::shared_ptr<broclip::ClipboardManager>& mgr);

void addClipEventListener(const std::string& event, Value callback);
void removeClipEventListener(const std::string& event, Value callback);
void dispatchClipEvent(std::string_view event, Value payload);

Value entryToJs(const ClipEntry& entry);
Value entryToHistoryJs(const ClipEntry& entry);

} // namespace broclip::api
