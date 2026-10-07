#include "../src/api/api.h"
#include "embed/embed.h"
#include "eval/eval.h"
#include "broclip/clip.h"
#include "broclip/types.h"
#include "broclip/backend.h"

#include <memory>

#include <iostream>
#include <string>
#include <cstdlib>
#if defined(__unix__) || defined(__linux__) || defined(__APPLE__)
#include <csignal>
#endif

// The test's clipboard: selections land here, never on the machine's own
// clipboard, so running the test does not overwrite what the user copied.
class MemoryBackend : public broclip::ClipboardBackend {
public:
    bool is_available() const override { return true; }
    const char* name() const override { return "memory"; }
    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }
    bool is_running() const override { return running_; }
    bool set_selection(broclip::SelectionKind, const broclip::ClipEntry&) override { return true; }
    bool clear_selection(broclip::SelectionKind) override { return true; }
    void set_on_selection_change(SelectionChangeHandler) override {}
    void set_on_selection_clear(SelectionClearHandler) override {}

private:
    bool running_ = false;
};

#define CHECK(cond)                                                          \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "CHECK failed: " #cond " (" << __FILE__ << ":"        \
                      << __LINE__ << ")" << std::endl;                         \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

int main() {
#if defined(__unix__) || defined(__linux__) || defined(__APPLE__)
    std::signal(SIGPIPE, SIG_IGN);
#endif
    namespace ev = bronze::embed;
    using namespace bronze::eval;

    std::cout << "Starting broclip JavaScript API tests..." << std::endl;

    // 1. Mount bro.clip into Bronze realm
    std::cout << "1. Mounting bro.clip into Bronze realm..." << std::endl;
    {
        auto mgr = std::make_shared<broclip::ClipboardManager>(
            broclip::ManagerOptions{}, nullptr, std::make_unique<MemoryBackend>());
        mgr->start();
        broclip::api::setClipboardManager(mgr);
    }
    broclip::api::installClip();

    auto g = ev::globalValue("bro");
    CHECK(g.found);
    CHECK(ev::isObject(g.value));

    ev::Persistent clip(ev::getProperty(g.value, "clip"));
    CHECK(ev::isObject(clip.get()));
    std::cout << "  Mounted bro.clip successfully." << std::endl;

    // Verify all required methods exist
    const char* required_methods[] = {
        "getHistory", "getEntry", "getText", "setText",
        "setPinned", "remove", "clear", "paste", "copyEntry",
        "on", "off", "addEventListener", "removeEventListener"
    };
    for (const char* m : required_methods) {
        auto fn = ev::getProperty(clip.get(), m);
        CHECK(ev::isFunction(fn));
        std::cout << "  Found bro.clip." << m << " [PASS]" << std::endl;
    }

    // 2. Test setText and getText on clipboard and primary
    std::cout << "2. Testing setText and getText..." << std::endl;
    {
        auto r = evalScript(
            "(function() {\n"
            "  const ok = bro.clip.setText('hello');\n"
            "  if (ok !== true) return 'setText failed';\n"
            "  const t = bro.clip.getText();\n"
            "  if (t !== 'hello') return 'getText mismatch: got ' + t;\n"
            "  const okP = bro.clip.setText('world', 'primary');\n"
            "  if (okP !== true) return 'setText primary failed';\n"
            "  const pText = bro.clip.getText('primary');\n"
            "  if (pText !== 'world') return 'getText primary mismatch: got ' + pText;\n"
            "  const cText = bro.clip.getText('clipboard');\n"
            "  if (cText !== 'hello') return 'getText clipboard mismatch: got ' + cText;\n"
            "  return true;\n"
            "})()\n"
        );
        if (r.thrown) {
            std::cerr << "Script threw: " << ev::toUtf8(r.value) << std::endl;
        }
        CHECK(!r.thrown);
        CHECK(ev::isBool(r.value) && ev::toBool(r.value));
        std::cout << "  setText and getText [PASS]" << std::endl;
    }

    // 3. Test getHistory() shape and query options
    std::cout << "3. Testing getHistory()..." << std::endl;
    {
        auto r = evalScript(
            "(function() {\n"
            "  const history = bro.clip.getHistory();\n"
            "  if (!Array.isArray(history) || history.length === 0) return 'expected array';\n"
            "  const item = history[0];\n"
            "  if (typeof item.id !== 'number' || item.id <= 0) return 'invalid id';\n"
            "  if (typeof item.timestamp !== 'number') return 'invalid timestamp';\n"
            "  if (typeof item.source !== 'string') return 'invalid source';\n"
            "  if (typeof item.isPinned !== 'boolean') return 'invalid isPinned';\n"
            "  if (typeof item.previewText !== 'string') return 'invalid previewText';\n"
            "  if (!Array.isArray(item.mimeTypes) || item.mimeTypes.length === 0) return 'invalid mimeTypes';\n"
            "  if (typeof item.byteSize !== 'number' || item.byteSize <= 0) return 'invalid byteSize';\n"
            "  return true;\n"
            "})()\n"
        );
        if (r.thrown) {
            std::cerr << "Script threw: " << ev::toUtf8(r.value) << std::endl;
        }
        CHECK(!r.thrown);
        CHECK(ev::isBool(r.value) && ev::toBool(r.value));

        auto rFilter = evalScript(
            "(function() {\n"
            "  const filtered = bro.clip.getHistory({ filterText: 'world' });\n"
            "  if (!Array.isArray(filtered) || filtered.length === 0) return 'no match';\n"
            "  if (!filtered[0].previewText.includes('world')) return 'preview mismatch';\n"
            "  const limit1 = bro.clip.getHistory({ maxResults: 1 });\n"
            "  if (limit1.length !== 1) return 'limit failed';\n"
            "  return true;\n"
            "})()\n"
        );
        CHECK(!rFilter.thrown);
        CHECK(ev::isBool(rFilter.value) && ev::toBool(rFilter.value));
        std::cout << "  getHistory() [PASS]" << std::endl;
    }

    // 4. Test getEntry(id) and getText(id)
    std::cout << "4. Testing getEntry(id) and getText(id)..." << std::endl;
    {
        auto r = evalScript(
            "(function() {\n"
            "  const history = bro.clip.getHistory();\n"
            "  const id = history[0].id;\n"
            "  const entry = bro.clip.getEntry(id);\n"
            "  if (!entry || typeof entry !== 'object') return 'entry not object';\n"
            "  if (entry.id !== id) return 'entry id mismatch';\n"
            "  if (typeof entry.text !== 'string' || entry.text.length === 0) return 'missing text';\n"
            "  const textById = bro.clip.getText(id);\n"
            "  if (textById !== entry.text) return 'getText(id) mismatch';\n"
            "  if (!entry.types || typeof entry.types !== 'object') return 'missing types object';\n"
            "  if (!Array.isArray(entry.mimeTypes)) return 'missing mimeTypes array';\n"
            "  const nullEntry = bro.clip.getEntry(99999999);\n"
            "  if (nullEntry !== null) return 'expected null for non-existent entry';\n"
            "  return true;\n"
            "})()\n"
        );
        if (r.thrown) {
            std::cerr << "Script threw: " << ev::toUtf8(r.value) << std::endl;
        }
        CHECK(!r.thrown);
        CHECK(ev::isBool(r.value) && ev::toBool(r.value));
        std::cout << "  getEntry(id) and getText(id) [PASS]" << std::endl;
    }

    // 5. Test setPinned(id, isPinned)
    std::cout << "5. Testing setPinned()..." << std::endl;
    {
        auto r = evalScript(
            "(function() {\n"
            "  const history = bro.clip.getHistory();\n"
            "  const id = history[0].id;\n"
            "  if (!bro.clip.setPinned(id, true)) return 'setPinned true failed';\n"
            "  let entry = bro.clip.getEntry(id);\n"
            "  if (entry.isPinned !== true) return 'expected isPinned === true';\n"
            "  let pinnedOnly = bro.clip.getHistory({ pinnedOnly: true });\n"
            "  if (pinnedOnly.length === 0) return 'expected pinned items';\n"
            "  if (!bro.clip.setPinned(id, false)) return 'setPinned false failed';\n"
            "  entry = bro.clip.getEntry(id);\n"
            "  if (entry.isPinned !== false) return 'expected isPinned === false';\n"
            "  return true;\n"
            "})()\n"
        );
        CHECK(!r.thrown);
        CHECK(ev::isBool(r.value) && ev::toBool(r.value));
        std::cout << "  setPinned() [PASS]" << std::endl;
    }

    // 6. Test paste(id) / copyEntry(id)
    std::cout << "6. Testing paste(id) and copyEntry(id)..." << std::endl;
    {
        auto r = evalScript(
            "(function() {\n"
            "  bro.clip.setText('first test entry');\n"
            "  bro.clip.setText('second test entry');\n"
            "  const history = bro.clip.getHistory();\n"
            "  const first = history.find(e => e.previewText.includes('first test entry'));\n"
            "  if (!first) return 'first entry not in history';\n"
            "  if (!bro.clip.paste(first.id)) return 'paste failed';\n"
            "  if (bro.clip.getText() !== 'first test entry') return 'paste did not activate entry';\n"
            "  const second = history.find(e => e.previewText.includes('second test entry'));\n"
            "  if (!second) return 'second entry not in history';\n"
            "  if (!bro.clip.copyEntry(second.id)) return 'copyEntry failed';\n"
            "  if (bro.clip.getText() !== 'second test entry') return 'copyEntry did not activate entry';\n"
            "  return true;\n"
            "})()\n"
        );
        CHECK(!r.thrown);
        CHECK(ev::isBool(r.value) && ev::toBool(r.value));
        std::cout << "  paste(id) and copyEntry(id) [PASS]" << std::endl;
    }

    // 7. Test remove(id)
    std::cout << "7. Testing remove(id)..." << std::endl;
    {
        auto r = evalScript(
            "(function() {\n"
            "  bro.clip.setText('remove me target');\n"
            "  const match = bro.clip.getHistory({ filterText: 'remove me target' });\n"
            "  if (match.length === 0) return 'target not found';\n"
            "  const id = match[0].id;\n"
            "  if (!bro.clip.remove(id)) return 'remove failed';\n"
            "  if (bro.clip.getEntry(id) !== null) return 'entry still exists after remove';\n"
            "  return true;\n"
            "})()\n"
        );
        CHECK(!r.thrown);
        CHECK(ev::isBool(r.value) && ev::toBool(r.value));
        std::cout << "  remove(id) [PASS]" << std::endl;
    }

    // 8. Test event listeners: on, off, addEventListener, removeEventListener, tickClipAsync
    std::cout << "8. Testing event listeners and tickClipAsync()..." << std::endl;
    {
        // Drain any pending events accumulated from earlier steps
        broclip::api::tickClipAsync();

        evalScript(
            "globalThis._clipEvents = [];\n"
            "globalThis._clearEvents = [];\n"
            "globalThis._clipCb = function(e) { globalThis._clipEvents.push(e); };\n"
            "globalThis._clearCb = function(e) { globalThis._clearEvents.push(e); };\n"
            "bro.clip.on('clip', globalThis._clipCb);\n"
            "bro.clip.addEventListener('clear', globalThis._clearCb);\n"
        );

        // Perform setText
        evalScript("bro.clip.setText('async event payload');");

        // Tick events
        broclip::api::tickClipAsync();

        auto rClipEvent = evalScript(
            "(function() {\n"
            "  if (globalThis._clipEvents.length !== 1) return 'expected 1 clip event, got ' + globalThis._clipEvents.length;\n"
            "  const ev = globalThis._clipEvents[0];\n"
            "  if (!ev || typeof ev !== 'object') return 'event not object';\n"
            "  if (ev.text !== 'async event payload') return 'event text mismatch: ' + ev.text;\n"
            "  return true;\n"
            "})()\n"
        );
        if (rClipEvent.thrown || !ev::isBool(rClipEvent.value) || !ev::toBool(rClipEvent.value)) {
            std::cerr << "rClipEvent returned: " << ev::toUtf8(rClipEvent.value) << std::endl;
        }
        CHECK(!rClipEvent.thrown);
        CHECK(ev::isBool(rClipEvent.value) && ev::toBool(rClipEvent.value));

        // Perform clear
        evalScript("bro.clip.clear('clipboard');");
        broclip::api::tickClipAsync();

        auto rClearEvent = evalScript(
            "(function() {\n"
            "  if (globalThis._clearEvents.length !== 1) return 'expected 1 clear event, got ' + globalThis._clearEvents.length;\n"
            "  return true;\n"
            "})()\n"
        );
        CHECK(!rClearEvent.thrown);
        CHECK(ev::isBool(rClearEvent.value) && ev::toBool(rClearEvent.value));

        // Test unregistering
        evalScript(
            "bro.clip.off('clip', globalThis._clipCb);\n"
            "bro.clip.removeEventListener('clear', globalThis._clearCb);\n"
            "bro.clip.setText('after off test');\n"
            "bro.clip.clear();\n"
        );
        broclip::api::tickClipAsync();

        auto rAfterOff = evalScript(
            "(function() {\n"
            "  if (globalThis._clipEvents.length !== 1) return 'clip event fired after off';\n"
            "  if (globalThis._clearEvents.length !== 1) return 'clear event fired after removeEventListener';\n"
            "  return true;\n"
            "})()\n"
        );
        CHECK(!rAfterOff.thrown);
        CHECK(ev::isBool(rAfterOff.value) && ev::toBool(rAfterOff.value));
        std::cout << "  event listeners and tickClipAsync() [PASS]" << std::endl;
    }

    // 9. Test clear() and history emptiness
    std::cout << "9. Testing clear()..." << std::endl;
    {
        evalScript("bro.clip.clear();");
        auto r = evalScript("bro.clip.getHistory().length === 0;");
        CHECK(!r.thrown);
        CHECK(ev::isBool(r.value) && ev::toBool(r.value));
        std::cout << "  clear() [PASS]" << std::endl;
    }

    // 10. Test shutdownClipAsync()
    std::cout << "10. Testing shutdownClipAsync()..." << std::endl;
    {
        broclip::api::shutdownClipAsync();
        std::cout << "  shutdownClipAsync() [PASS]" << std::endl;
    }

    // 11. GC stress loop
    std::cout << "11. Running GC stress loop..." << std::endl;
    {
        auto rStress = evalScript(
            "(function() {\n"
            "  for (let i = 0; i < 200; ++i) {\n"
            "    bro.clip.setText('stress test message ' + i);\n"
            "    const h = bro.clip.getHistory();\n"
            "    if (h.length > 0) {\n"
            "      const e = bro.clip.getEntry(h[0].id);\n"
            "      const t = bro.clip.getText(h[0].id);\n"
            "    }\n"
            "    const cb = function(e) {};\n"
            "    bro.clip.on('clip', cb);\n"
            "    bro.clip.off('clip', cb);\n"
            "  }\n"
            "  return true;\n"
            "})()\n"
        );
        CHECK(!rStress.thrown);
        CHECK(ev::isBool(rStress.value) && ev::toBool(rStress.value));
        broclip::api::tickClipAsync();
        std::cout << "  GC stress loop [PASS]" << std::endl;
    }

    std::cout << "All broclip JavaScript API tests PASSED!" << std::endl;
    return 0;
}
