#include "host_clip_internal.h"
#include "arg_reader.h"
#include "object_builder.h"
#include "broclip/clip.h"
#include "broclip/types.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace broclip::api {

namespace {

struct Subscription {
    std::string event;
    std::shared_ptr<ev::Persistent> callback;
};

struct QueuedClipEvent {
    enum Type { Clip, Clear } type;
    ClipEntry entry;
    SelectionKind selection_kind;
};

std::mutex g_sub_mu;
std::vector<Subscription> g_subscriptions;

std::mutex g_event_queue_mu;
std::vector<QueuedClipEvent> g_pending_events;

ListenerSlot g_manager_clip_slot;
ListenerSlot g_manager_clear_slot;
std::shared_ptr<broclip::ClipboardManager> g_connected_manager;

std::optional<SelectionKind> parseSelectionKind(Value val) {
    if (ev::isUndefined(val) || ev::isNull(val)) return std::nullopt;
    if (ev::isString(val)) {
        std::string s = ev::toUtf8(val);
        if (s == "primary" || s == "Primary" || s == "1") return SelectionKind::Primary;
        if (s == "clipboard" || s == "Clipboard" || s == "0") return SelectionKind::Clipboard;
    } else if (ev::isNumber(val)) {
        int k = static_cast<int>(ev::toDouble(val));
        if (k == 1) return SelectionKind::Primary;
        if (k == 0) return SelectionKind::Clipboard;
    }
    return std::nullopt;
}

HistoryQuery parseHistoryQuery(Value optsVal) {
    HistoryQuery q;
    if (!ev::isObject(optsVal)) return q;

    ev::Persistent opts(optsVal);

    Value filterVal = ev::getProperty(opts.get(), "filterText");
    if (ev::isUndefined(filterVal)) filterVal = ev::getProperty(opts.get(), "filter_text");
    if (ev::isUndefined(filterVal)) filterVal = ev::getProperty(opts.get(), "text");
    if (ev::isString(filterVal)) {
        q.filter_text = ev::toUtf8(filterVal);
    }

    Value mimeVal = ev::getProperty(opts.get(), "mimeFilter");
    if (ev::isUndefined(mimeVal)) mimeVal = ev::getProperty(opts.get(), "mime_filter");
    if (ev::isUndefined(mimeVal)) mimeVal = ev::getProperty(opts.get(), "mime");
    if (ev::isString(mimeVal)) {
        q.mime_filter = ev::toUtf8(mimeVal);
    }

    Value kindVal = ev::getProperty(opts.get(), "selectionKind");
    if (ev::isUndefined(kindVal)) kindVal = ev::getProperty(opts.get(), "selection_kind");
    if (ev::isUndefined(kindVal)) kindVal = ev::getProperty(opts.get(), "kind");
    q.selection_kind = parseSelectionKind(kindVal);

    Value pinnedVal = ev::getProperty(opts.get(), "pinnedOnly");
    if (ev::isUndefined(pinnedVal)) pinnedVal = ev::getProperty(opts.get(), "pinned_only");
    if (ev::isUndefined(pinnedVal)) pinnedVal = ev::getProperty(opts.get(), "pinned");
    if (ev::isBool(pinnedVal)) {
        q.pinned_only = ev::toBool(pinnedVal);
    }

    Value sinceVal = ev::getProperty(opts.get(), "since");
    if (ev::isNumber(sinceVal)) {
        double d = ev::toDouble(sinceVal);
        if (d > 0) {
            q.since = std::chrono::system_clock::time_point(
                std::chrono::milliseconds(static_cast<int64_t>(d)));
        }
    }

    Value maxVal = ev::getProperty(opts.get(), "maxResults");
    if (ev::isUndefined(maxVal)) maxVal = ev::getProperty(opts.get(), "max_results");
    if (ev::isUndefined(maxVal)) maxVal = ev::getProperty(opts.get(), "limit");
    if (ev::isNumber(maxVal)) {
        double d = ev::toDouble(maxVal);
        if (d >= 0) q.max_results = static_cast<size_t>(d);
    }

    Value offsetVal = ev::getProperty(opts.get(), "offset");
    if (ev::isNumber(offsetVal)) {
        double d = ev::toDouble(offsetVal);
        if (d >= 0) q.offset = static_cast<size_t>(d);
    }

    return q;
}

} // namespace

void connectManager(const std::shared_ptr<broclip::ClipboardManager>& mgr) {
    std::lock_guard lock(g_sub_mu);
    if (g_connected_manager == mgr && g_manager_clip_slot.is_connected()) return;
    g_manager_clip_slot.disconnect();
    g_manager_clear_slot.disconnect();
    g_connected_manager = mgr;
    if (mgr) {
        g_manager_clip_slot = mgr->on_clip([](const ClipEntry& entry) {
            std::lock_guard qlock(g_event_queue_mu);
            g_pending_events.push_back(QueuedClipEvent{
                .type = QueuedClipEvent::Clip,
                .entry = entry,
                .selection_kind = entry.kind,
            });
        });
        g_manager_clear_slot = mgr->on_clear([](SelectionKind kind) {
            std::lock_guard qlock(g_event_queue_mu);
            g_pending_events.push_back(QueuedClipEvent{
                .type = QueuedClipEvent::Clear,
                .entry = {},
                .selection_kind = kind,
            });
        });
    }
}

void disconnectManagerSlots() {
    std::lock_guard lock(g_sub_mu);
    g_manager_clip_slot.disconnect();
    g_manager_clear_slot.disconnect();
    g_connected_manager.reset();
    {
        std::lock_guard qlock(g_event_queue_mu);
        g_pending_events.clear();
    }
}

void clearClipSubscriptions() {
    std::lock_guard lock(g_sub_mu);
    g_subscriptions.clear();
}

void addClipEventListener(const std::string& event, Value callback) {
    if (!ev::isFunction(callback)) return;
    std::lock_guard lock(g_sub_mu);
    Subscription sub;
    sub.event = event;
    sub.callback = std::make_shared<ev::Persistent>(callback);
    g_subscriptions.push_back(std::move(sub));
}

void removeClipEventListener(const std::string& event, Value callback) {
    if (!ev::isFunction(callback)) return;
    std::lock_guard lock(g_sub_mu);
    g_subscriptions.erase(
        std::remove_if(g_subscriptions.begin(), g_subscriptions.end(),
                       [&](const Subscription& sub) {
                           if (sub.event != event) return false;
                           return sub.callback && sub.callback->get() == callback;
                       }),
        g_subscriptions.end());
}

void dispatchClipEvent(std::string_view event, Value payload) {
    ev::Persistent payloadRoot(payload);

    std::vector<std::shared_ptr<ev::Persistent>> targets;
    {
        std::lock_guard lock(g_sub_mu);
        for (const auto& sub : g_subscriptions) {
            if (sub.event == event || sub.event == "*" || sub.event == "change") {
                if (sub.callback) {
                    targets.push_back(sub.callback);
                }
            }
        }
    }

    for (const auto& cb : targets) {
        if (!cb || !ev::isFunction(cb->get())) continue;
        const Value arg = payloadRoot.get();
        ev::catchThrow([&]() {
            return ev::call(cb->get(), ev::undefined(), std::span<const Value>(&arg, 1)).value;
        });
    }

    ev::Persistent clipObj(ensureBroClip());
    if (ev::isObject(clipObj.get())) {
        std::string propName = "on" + std::string(event);
        ev::Persistent handler(ev::getProperty(clipObj.get(), propName));
        if (ev::isFunction(handler.get())) {
            const Value arg = payloadRoot.get();
            ev::catchThrow([&]() {
                return ev::call(handler.get(), clipObj.get(), std::span<const Value>(&arg, 1)).value;
            });
        }
    }
}

void drainClipEvents() {
    auto mgr = activeClipboardManager();
    if (mgr) {
        connectManager(mgr);
    }

    std::vector<QueuedClipEvent> events;
    {
        std::lock_guard lock(g_event_queue_mu);
        events.swap(g_pending_events);
    }

    for (const auto& evItem : events) {
        if (evItem.type == QueuedClipEvent::Clip) {
            Value payload = entryToJs(evItem.entry);
            dispatchClipEvent("clip", payload);
        } else if (evItem.type == QueuedClipEvent::Clear) {
            ObjectBuilder b;
            b.set("type", "clear");
            b.set("selectionKind", std::string(to_string(evItem.selection_kind)));
            b.set("kind", std::string(to_string(evItem.selection_kind)));
            dispatchClipEvent("clear", b.build());
        }
    }

    ev::drainMicrotasks();
}

Value entryToHistoryJs(const ClipEntry& entry) {
    ObjectBuilder b;
    b.set("id", static_cast<double>(entry.id));

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        entry.timestamp.time_since_epoch()).count();
    b.set("timestamp", static_cast<double>(ms));

    std::string sourceStr = std::string(to_string(entry.source.kind));
    if (!entry.source.app_name.empty()) {
        b.set("appName", entry.source.app_name);
        b.set("app_name", entry.source.app_name);
    }
    b.set("source", sourceStr);

    b.set("isPinned", entry.pinned);
    b.set("pinned", entry.pinned);

    b.set("previewText", entry.preview());
    b.set("preview", entry.preview());

    ArrayBuilder mimes;
    for (const auto& p : entry.payloads) {
        mimes.push(ev::fromUtf8(p.mime));
    }
    b.set("mimeTypes", mimes.build());
    b.set("mime_types", mimes.build());

    b.set("byteSize", static_cast<double>(entry.size_bytes()));
    b.set("byte_size", static_cast<double>(entry.size_bytes()));

    b.set("kind", std::string(to_string(entry.kind)));
    b.set("selectionKind", std::string(to_string(entry.kind)));

    auto textOpt = entry.text();
    if (textOpt) {
        b.set("text", *textOpt);
    }

    return b.build();
}

Value entryToJs(const ClipEntry& entry) {
    ObjectBuilder b;
    b.set("id", static_cast<double>(entry.id));

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        entry.timestamp.time_since_epoch()).count();
    b.set("timestamp", static_cast<double>(ms));

    std::string sourceStr = std::string(to_string(entry.source.kind));
    if (!entry.source.app_name.empty()) {
        b.set("appName", entry.source.app_name);
        b.set("app_name", entry.source.app_name);
    }
    b.set("source", sourceStr);

    b.set("isPinned", entry.pinned);
    b.set("pinned", entry.pinned);

    b.set("previewText", entry.preview());
    b.set("preview", entry.preview());

    b.set("byteSize", static_cast<double>(entry.size_bytes()));
    b.set("byte_size", static_cast<double>(entry.size_bytes()));

    b.set("kind", std::string(to_string(entry.kind)));
    b.set("selectionKind", std::string(to_string(entry.kind)));

    auto textOpt = entry.text();
    if (textOpt) {
        b.set("text", *textOpt);
    } else {
        b.set("text", ev::null());
    }

    ArrayBuilder mimes;
    ObjectBuilder typesObj;
    ObjectBuilder dataObj;
    ArrayBuilder payloadsArr;

    for (const auto& p : entry.payloads) {
        mimes.push(ev::fromUtf8(p.mime));

        ObjectBuilder payloadObj;
        payloadObj.set("mime", p.mime);
        payloadObj.set("size", static_cast<double>(p.size()));

        if (mime::is_text(p.mime) || p.mime.starts_with("text/") || p.mime == "STRING" || p.mime == "UTF8_STRING") {
            std::string s = p.as_string();
            payloadObj.set("text", s);
            typesObj.set(p.mime, s);
            dataObj.set(p.mime, s);
            if (p.mime == mime::kTextPlainUtf8) {
                typesObj.set(mime::kTextPlain, s);
                dataObj.set(mime::kTextPlain, s);
            }
        } else {
            Value buf = ev::createArrayBuffer(std::span<const uint8_t>(p.data.data(), p.data.size()));
            payloadObj.set("data", buf);
            typesObj.set(p.mime, buf);
            dataObj.set(p.mime, buf);
        }
        payloadsArr.push(payloadObj.build());
    }

    b.set("mimeTypes", mimes.build());
    b.set("mime_types", mimes.build());
    b.set("types", typesObj.build());
    b.set("data", dataObj.build());
    b.set("payloads", payloadsArr.build());

    return b.build();
}

void installClipOnto(Value clipVal) {
    ObjectBuilder clip(clipVal);

    // bro.clip.getHistory(queryOptions) -> array of { id, timestamp, source, isPinned, previewText, mimeTypes, byteSize }
    clip.def("getHistory", 1, [](Value, std::span<const Value> args) -> Value {
        auto mgr = activeClipboardManager();
        if (!mgr) return ev::makeArray(0);

        HistoryQuery q;
        if (!args.empty() && ev::isObject(args[0])) {
            q = parseHistoryQuery(args[0]);
        }

        auto entries = mgr->history(q);
        ArrayBuilder arr;
        for (const auto& entry : entries) {
            arr.push(entryToHistoryJs(entry));
        }
        return arr.build();
    });

    // bro.clip.getEntry(id) -> entry object with all MIME types
    clip.def("getEntry", 1, [](Value, std::span<const Value> args) -> Value {
        if (args.empty()) return ev::null();
        auto mgr = activeClipboardManager();
        if (!mgr) return ev::null();

        uint64_t id = 0;
        if (ev::isNumber(args[0])) {
            double d = ev::toDouble(args[0]);
            if (d >= 0) id = static_cast<uint64_t>(d);
        } else if (ev::isString(args[0])) {
            try {
                id = std::stoull(ev::toUtf8(args[0]));
            } catch (...) {
                return ev::null();
            }
        } else {
            return ev::null();
        }

        auto entryOpt = mgr->get(id);
        if (!entryOpt) return ev::null();
        return entryToJs(*entryOpt);
    });

    // bro.clip.getText(id) -> string text content of the entry
    clip.def("getText", 1, [](Value, std::span<const Value> args) -> Value {
        auto mgr = activeClipboardManager();
        if (!mgr) return ev::fromUtf8("");

        if (args.empty() || ev::isUndefined(args[0]) || ev::isNull(args[0])) {
            auto cur = mgr->current(SelectionKind::Clipboard);
            if (cur && cur->text()) return ev::fromUtf8(*cur->text());
            return ev::fromUtf8("");
        }

        if (ev::isString(args[0])) {
            std::string s = ev::toUtf8(args[0]);
            if (s == "primary" || s == "Primary") {
                auto cur = mgr->current(SelectionKind::Primary);
                if (cur && cur->text()) return ev::fromUtf8(*cur->text());
                return ev::fromUtf8("");
            }
            if (s == "clipboard" || s == "Clipboard") {
                auto cur = mgr->current(SelectionKind::Clipboard);
                if (cur && cur->text()) return ev::fromUtf8(*cur->text());
                return ev::fromUtf8("");
            }
            try {
                uint64_t id = std::stoull(s);
                auto e = mgr->get(id);
                if (e && e->text()) return ev::fromUtf8(*e->text());
            } catch (...) {}
            return ev::fromUtf8("");
        }

        if (ev::isNumber(args[0])) {
            double d = ev::toDouble(args[0]);
            if (d >= 0) {
                auto e = mgr->get(static_cast<uint64_t>(d));
                if (e && e->text()) return ev::fromUtf8(*e->text());
            }
        }

        return ev::fromUtf8("");
    });

    // bro.clip.setText(text, selectionKind) -> sets text on clipboard/primary
    clip.def("setText", 2, [](Value, std::span<const Value> args) -> Value {
        if (args.empty()) return ev::fromBool(false);
        auto mgr = activeClipboardManager();
        if (!mgr) return ev::fromBool(false);

        std::string text;
        if (ev::isString(args[0])) {
            text = ev::toUtf8(args[0]);
        } else if (!ev::isUndefined(args[0]) && !ev::isNull(args[0])) {
            text = ev::toUtf8(args[0]);
        }

        SelectionKind kind = SelectionKind::Clipboard;
        if (args.size() > 1) {
            auto k = parseSelectionKind(args[1]);
            if (k) kind = *k;
        }

        bool ok = mgr->set_text(text, kind);
        return ev::fromBool(ok);
    });

    // bro.clip.setPinned(id, isPinned) -> pins/unpins entry in LRU ring
    clip.def("setPinned", 2, [](Value, std::span<const Value> args) -> Value {
        if (args.empty()) return ev::fromBool(false);
        auto mgr = activeClipboardManager();
        if (!mgr) return ev::fromBool(false);

        uint64_t id = 0;
        if (ev::isNumber(args[0])) {
            double d = ev::toDouble(args[0]);
            if (d >= 0) id = static_cast<uint64_t>(d);
        } else if (ev::isString(args[0])) {
            try {
                id = std::stoull(ev::toUtf8(args[0]));
            } catch (...) {
                return ev::fromBool(false);
            }
        } else {
            return ev::fromBool(false);
        }

        bool isPinned = true;
        if (args.size() > 1 && ev::isBool(args[1])) {
            isPinned = ev::toBool(args[1]);
        }

        bool ok = mgr->set_pinned(id, isPinned);
        return ev::fromBool(ok);
    });

    // bro.clip.remove(id) -> removes entry
    clip.def("remove", 1, [](Value, std::span<const Value> args) -> Value {
        if (args.empty()) return ev::fromBool(false);
        auto mgr = activeClipboardManager();
        if (!mgr) return ev::fromBool(false);

        uint64_t id = 0;
        if (ev::isNumber(args[0])) {
            double d = ev::toDouble(args[0]);
            if (d >= 0) id = static_cast<uint64_t>(d);
        } else if (ev::isString(args[0])) {
            try {
                id = std::stoull(ev::toUtf8(args[0]));
            } catch (...) {
                return ev::fromBool(false);
            }
        } else {
            return ev::fromBool(false);
        }

        bool ok = mgr->remove(id);
        return ev::fromBool(ok);
    });

    // bro.clip.clear(selectionKind) -> clears clipboard history
    clip.def("clear", 1, [](Value, std::span<const Value> args) -> Value {
        auto mgr = activeClipboardManager();
        if (!mgr) return ev::fromBool(false);

        std::optional<SelectionKind> kind;
        if (!args.empty()) {
            kind = parseSelectionKind(args[0]);
        }

        mgr->clear(kind);
        return ev::fromBool(true);
    });

    // bro.clip.paste(id) / copyEntry(id) -> copies existing entry to active clipboard selection
    auto pasteFn = [](Value, std::span<const Value> args) -> Value {
        if (args.empty()) return ev::fromBool(false);
        auto mgr = activeClipboardManager();
        if (!mgr) return ev::fromBool(false);

        uint64_t id = 0;
        if (ev::isNumber(args[0])) {
            double d = ev::toDouble(args[0]);
            if (d >= 0) id = static_cast<uint64_t>(d);
        } else if (ev::isString(args[0])) {
            try {
                id = std::stoull(ev::toUtf8(args[0]));
            } catch (...) {
                return ev::fromBool(false);
            }
        } else {
            return ev::fromBool(false);
        }

        auto entryOpt = mgr->get(id);
        if (!entryOpt) return ev::fromBool(false);

        ClipEntry entry = *entryOpt;
        entry.id = 0;
        entry.timestamp = std::chrono::system_clock::now();

        if (args.size() > 1) {
            auto k = parseSelectionKind(args[1]);
            if (k) entry.kind = *k;
        }

        bool ok = mgr->set_clip(std::move(entry));
        return ev::fromBool(ok);
    };

    clip.def("paste", 2, pasteFn);
    clip.def("copyEntry", 2, pasteFn);

    // bro.clip.on(event, cb)
    auto onFn = [](Value self, std::span<const Value> args) -> Value {
        if (args.size() < 2) return self;
        ev::Persistent arg0(args[0]);
        ev::Persistent arg1(args[1]);
        if (!ev::isString(arg0.get()) || !ev::isFunction(arg1.get())) return self;
        addClipEventListener(ev::toUtf8(arg0.get()), arg1.get());
        return self;
    };

    // bro.clip.off(event, cb)
    auto offFn = [](Value self, std::span<const Value> args) -> Value {
        if (args.size() < 2) return self;
        ev::Persistent arg0(args[0]);
        ev::Persistent arg1(args[1]);
        if (!ev::isString(arg0.get()) || !ev::isFunction(arg1.get())) return self;
        removeClipEventListener(ev::toUtf8(arg0.get()), arg1.get());
        return self;
    };

    clip.def("on", 2, onFn);
    clip.def("off", 2, offFn);
    clip.def("addEventListener", 2, onFn);
    clip.def("removeEventListener", 2, offFn);

    // bro.clip.current(selectionKind)
    clip.def("current", 1, [](Value, std::span<const Value> args) -> Value {
        auto mgr = activeClipboardManager();
        if (!mgr) return ev::null();
        SelectionKind kind = SelectionKind::Clipboard;
        if (!args.empty()) {
            auto k = parseSelectionKind(args[0]);
            if (k) kind = *k;
        }
        auto cur = mgr->current(kind);
        if (!cur) return ev::null();
        return entryToJs(*cur);
    });

    // bro.clip.count(selectionKind)
    clip.def("count", 1, [](Value, std::span<const Value> args) -> Value {
        auto mgr = activeClipboardManager();
        if (!mgr) return ev::fromDouble(0.0);
        std::optional<SelectionKind> kind;
        if (!args.empty()) {
            kind = parseSelectionKind(args[0]);
        }
        return ev::fromDouble(static_cast<double>(mgr->count(kind)));
    });
}

} // namespace broclip::api
