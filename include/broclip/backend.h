#pragma once

#include "broclip/types.h"

#include <functional>
#include <memory>

namespace broclip {

class ClipboardBackend {
public:
    virtual ~ClipboardBackend() = default;

    virtual bool is_available() const = 0;
    virtual const char* name() const = 0;

    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool is_running() const = 0;

    virtual bool set_selection(SelectionKind kind, const ClipEntry& entry) = 0;
    virtual bool clear_selection(SelectionKind kind) = 0;

    using SelectionChangeHandler = std::function<void(ClipEntry)>;
    using SelectionClearHandler = std::function<void(SelectionKind)>;

    virtual void set_on_selection_change(SelectionChangeHandler handler) = 0;
    virtual void set_on_selection_clear(SelectionClearHandler handler) = 0;
};

std::unique_ptr<ClipboardBackend> create_platform_backend();

}  // namespace broclip
