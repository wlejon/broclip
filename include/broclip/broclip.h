#pragma once

#include "broclip/backend.h"
#include "broclip/clip.h"
#include "broclip/mime_stream.h"
#include "broclip/ring_store.h"
#include "broclip/types.h"

#if defined(__linux__)
#include "broclip/linux/data_control.h"
#elif defined(_WIN32)
#include "broclip/win/clipboard.h"
#elif defined(__APPLE__)
#include "broclip/mac/pasteboard.h"
#endif
