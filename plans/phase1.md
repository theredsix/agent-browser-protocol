# Phase 1: Complete Core Input Actions - COMPLETED

**Status: FULLY IMPLEMENTED**

All features in this phase have been implemented and tested.

## Goal

Complete the input layer to enable full browser control. After this phase, agents can perform all basic browser interactions: clicking, typing, scrolling, key combinations, and tab switching.

## Endpoints to Implement

### 1. Tab Activate
```
POST /tabs/{tab_id}/activate
```

Switch to a different tab.

**Implementation:**
- Use `TabStripModel::ActivateTabAt()` or `Browser::ActivateTabAt()`
- Find tab index from DevToolsAgentHost ID
- Activate the tab
- Return success with tab info

**Files to modify:**
- `abp_controller.cc` - Add `ActivateTab()` method
- Route in `HandleRequest()` for `activate` action

**CDP:** Not needed - direct Chrome API

---

### 2. Mouse Scroll
```
POST /tabs/{tab_id}/mouse/scroll
```

Scroll the page using mouse wheel events.

**Request:**
```json
{
  "x": 100,
  "y": 200,
  "delta_x": 0,
  "delta_y": -300
}
```

**Implementation:**
- Use CDP `Input.dispatchMouseEvent` with `type: "mouseWheel"`
- Set `deltaX` and `deltaY` parameters
- Negative `delta_y` scrolls down, positive scrolls up

**Files to modify:**
- `abp_controller.cc` - Add `Scroll()` method
- Route in `HandleRequest()` for `scroll` action

**CDP Command:**
```json
{
  "method": "Input.dispatchMouseEvent",
  "params": {
    "type": "mouseWheel",
    "x": 100,
    "y": 200,
    "deltaX": 0,
    "deltaY": -300
  }
}
```

---

### 3. Keyboard Press (with shortcuts)
```
POST /tabs/{tab_id}/keyboard/press
```

Press a key or key combination (keydown + keyup for all keys).

**Request:**
```json
{
  "key": "c",
  "modifiers": ["Control"]
}
```

**Implementation:**
- Convert modifiers array to CDP modifier flags
- For each modifier: send `keyDown`
- Send `keyDown` + `keyUp` for main key
- For each modifier (reverse): send `keyUp`
- Use `Input.dispatchKeyEvent`

**Modifier flag mapping:**
- `Alt` = 1
- `Control` = 2
- `Meta` = 4
- `Shift` = 8

**Files to modify:**
- `abp_controller.cc` - Add `KeyPress()` method
- Helper to convert key names to CDP key codes

**CDP Commands (for Ctrl+C):**
```json
// Control down
{"method": "Input.dispatchKeyEvent", "params": {"type": "keyDown", "key": "Control", "modifiers": 2}}
// C down
{"method": "Input.dispatchKeyEvent", "params": {"type": "keyDown", "key": "c", "modifiers": 2}}
// C up
{"method": "Input.dispatchKeyEvent", "params": {"type": "keyUp", "key": "c", "modifiers": 2}}
// Control up
{"method": "Input.dispatchKeyEvent", "params": {"type": "keyUp", "key": "Control", "modifiers": 0}}
```

---

### 4. Keyboard Down
```
POST /tabs/{tab_id}/keyboard/down
```

Press a key without releasing.

**Request:**
```json
{
  "key": "Shift",
  "modifiers": []
}
```

**Implementation:**
- Single `Input.dispatchKeyEvent` with `type: "keyDown"`
- Track held keys in controller state for modifier calculation

**Files to modify:**
- `abp_controller.cc` - Add `KeyDown()` method
- Add `held_keys_` map to track pressed keys per tab

---

### 5. Keyboard Up
```
POST /tabs/{tab_id}/keyboard/up
```

Release a pressed key.

**Request:**
```json
{
  "key": "Shift"
}
```

**Implementation:**
- Single `Input.dispatchKeyEvent` with `type: "keyUp"`
- Remove from held keys tracking

**Files to modify:**
- `abp_controller.cc` - Add `KeyUp()` method

---

### 6. Stop Loading
```
POST /tabs/{tab_id}/stop
```

Stop page loading.

**Implementation:**
- Use `WebContents::Stop()` directly
- No CDP needed

**Files to modify:**
- `abp_controller.cc` - Add `StopLoading()` method

---

## Helper Code Needed

### Key Code Mapping

Create a helper to map key names to CDP parameters:

```cpp
struct KeyInfo {
  std::string key;           // CDP key value
  std::string code;          // CDP code value
  int windows_virtual_key;   // windowsVirtualKeyCode
  int native_virtual_key;    // nativeVirtualKeyCode
};

KeyInfo GetKeyInfo(const std::string& key_name);
```

Common mappings:
- `"Enter"` → key: "Enter", code: "Enter", vk: 13
- `"Tab"` → key: "Tab", code: "Tab", vk: 9
- `"Escape"` → key: "Escape", code: "Escape", vk: 27
- `"ArrowUp"` → key: "ArrowUp", code: "ArrowUp", vk: 38
- `"a"` → key: "a", code: "KeyA", vk: 65

### Held Keys Tracking

```cpp
// In AbpController
struct HeldKeyState {
  std::set<std::string> held_keys;
  int current_modifiers = 0;  // Bitmask of active modifiers
};
std::map<std::string, HeldKeyState> held_keys_state_;
```

---

## Testing

### Manual Testing

```bash
# Tab activate
curl -X POST http://localhost:8222/api/v1/tabs/TAB_ID/activate

# Scroll down
curl -X POST http://localhost:8222/api/v1/tabs/TAB_ID/scroll \
  -H "Content-Type: application/json" \
  -d '{"x":500,"y":500,"delta_x":0,"delta_y":-300}'

# Press Enter
curl -X POST http://localhost:8222/api/v1/tabs/TAB_ID/keyboard/press \
  -H "Content-Type: application/json" \
  -d '{"key":"Enter"}'

# Copy (Ctrl+C)
curl -X POST http://localhost:8222/api/v1/tabs/TAB_ID/keyboard/press \
  -H "Content-Type: application/json" \
  -d '{"key":"c","modifiers":["Control"]}'

# Shift+Click (hold shift, click, release)
curl -X POST http://localhost:8222/api/v1/tabs/TAB_ID/keyboard/down \
  -d '{"key":"Shift"}'
curl -X POST http://localhost:8222/api/v1/tabs/TAB_ID/click \
  -d '{"x":100,"y":200}'
curl -X POST http://localhost:8222/api/v1/tabs/TAB_ID/keyboard/up \
  -d '{"key":"Shift"}'

# Stop loading
curl -X POST http://localhost:8222/api/v1/tabs/TAB_ID/stop
```

---

## Estimated Scope

| Task | Complexity | Lines of Code |
|------|------------|---------------|
| Tab activate | Low | ~30 |
| Mouse scroll | Low | ~40 |
| Key code mapping helper | Medium | ~150 |
| Keyboard press | Medium | ~80 |
| Keyboard down | Low | ~40 |
| Keyboard up | Low | ~30 |
| Stop loading | Low | ~20 |
| **Total** | | **~390** |

---

## Cleanup on Tab Close

When a tab is closed, any held key state for that tab must be cleaned up to prevent memory leaks.

**Implementation:**
- In `AbpController::CloseTab()`, add cleanup of held keys state
- Add `held_keys_state_.erase(tab_id)` before or after closing the tab

**Code change in `abp_controller.cc`:**
```cpp
void AbpController::CloseTab(const std::string& tab_id, ResponseCallback callback) {
  // ... existing tab lookup code ...

  // Clean up held keys state for this tab
  held_keys_state_.erase(tab_id);

  // ... existing close tab logic ...
}
```

This ensures that when a tab is closed (either via API or by the user), the `held_keys_state_` map does not retain stale entries.

---

## Success Criteria - ALL MET

After Phase 1:
- [x] Can switch between tabs via API (`POST /tabs/{id}/activate`)
- [x] Can scroll pages up/down/left/right (`POST /tabs/{id}/scroll`)
- [x] Can press individual keys (Enter, Tab, Escape, arrows, etc.) (`POST /tabs/{id}/keyboard/press`)
- [x] Can execute keyboard shortcuts (Ctrl+C, Ctrl+V, Ctrl+A, etc.) via modifiers array
- [x] Can hold modifier keys for multi-action sequences (`keyboard/down` + `keyboard/up`)
- [x] Can stop page loading (`POST /tabs/{id}/stop`)
- [x] All input actions integrate with existing history recording via AbpActionContext
- [x] Held keys state is cleaned up when tabs are closed (in `CloseTab()`)
