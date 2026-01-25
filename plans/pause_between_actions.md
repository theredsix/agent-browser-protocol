# V8 Virtual Clock + Debugger Pause for ABP Actions - IMPLEMENTED

**Status: FULLY IMPLEMENTED**

The execution control system is implemented in:
- `AbpActionContext` class (`abp_action_context.h/cc`) - Unified action flow with pause/resume
- `AbpController::execution_states_` - Per-tab execution state tracking
- `GET/POST /api/v1/tabs/{id}/execution` - API endpoints for manual control

Features implemented:
- [x] Virtual time policy control via CDP `Emulation.setVirtualTimePolicy`
- [x] Debugger pause/resume via CDP `Debugger.pause`/`Debugger.resume`
- [x] Automatic resume before actions, pause after
- [x] `--abp-disable-pause` flag to disable automatic pause
- [x] AbpActionContext for consistent action flow

---

## Goal
After each ABP input action (+ wait_until) completes:
1. **Pause V8 virtual clock** - Freezes timers, animations, Date.now()
2. **Debugger.pause** - Halts JavaScript execution entirely

Resume both when the next action starts. This provides complete deterministic page state between agent actions.

## Action Flow

```
Agent        ABP           CDP/V8
  |           |              |
  |--Click--->|              |
  |           |--Debugger.resume------------------->|  (unfreeze JS)
  |           |--setVirtualTimePolicy("advance")-->|  (resume clock)
  |           |--dispatchMouseEvent-->|
  |           |--[wait_until completes]----------->|
  |           |--setVirtualTimePolicy("pause")---->|  (freeze clock)
  |           |--Debugger.pause-------------------->|  (halt JS)
  |           |--captureScreenshot-->|
  |<--result--|              |
  |    (page completely frozen: no JS, no timers)
  |--Type---->|              |
  |           |--Debugger.resume------------------->|  (unfreeze JS)
  |           |--setVirtualTimePolicy("advance")-->|  (resume clock)
  ...
```

## Two Layers of Control

| Layer | CDP Command | What it controls |
|-------|-------------|------------------|
| **Execution** | `Debugger.pause` / `Debugger.resume` | Halts/resumes all JS execution |
| **Virtual Time** | `Emulation.setVirtualTimePolicy` | Freezes/advances timers, Date.now() |

Both are needed:
- Virtual time alone: JS still runs (event handlers, microtasks)
- Debugger pause alone: Time still advances when resumed (timers fire immediately)

## Implementation Phases

### Phase 1: AbpActionContext Class

**New file: `chrome/browser/abp/abp_action_context.h`**

A reusable container that wraps any state-modifying action with consistent:
- Execution resume at start
- wait_until handling
- Execution pause after completion
- Screenshot capture
- Response formatting

```cpp
// Callback for the actual action logic
using ActionCallback = base::OnceCallback<void(AbpActionContext* ctx)>;

class AbpActionContext : public base::RefCounted<AbpActionContext> {
 public:
  // Factory method - creates context and starts the action flow
  static void Run(AbpController* controller,
                  const std::string& tab_id,
                  const base::Value::Dict& params,
                  ActionCallback action,
                  ResponseCallback response);

  // Called by action implementation when action dispatch is complete
  void OnActionDispatched();

  // Called by action implementation to set result data
  void SetResult(base::Value::Dict result);

  // Access to CDP client for action implementation
  AbpCdpClient* client() { return client_; }
  content::WebContents* web_contents() { return web_contents_; }
  const base::Value::Dict& params() { return params_; }

 private:
  // Internal flow methods
  void Start();
  void OnExecutionResumed();
  void ExecuteAction();
  void OnWaitUntilComplete();
  void OnExecutionPaused();
  void CaptureScreenshot();
  void SendResponse();

  // State
  AbpController* controller_;
  std::string tab_id_;
  base::Value::Dict params_;
  ActionCallback action_;
  ResponseCallback response_callback_;
  AbpCdpClient* client_;
  content::WebContents* web_contents_;
  base::Value::Dict result_;

  // Timing
  base::TimeTicks action_start_;
  base::TimeTicks action_end_;
};
```

**Flow managed by AbpActionContext:**
```
Run()
  -> Start()
  -> ResumeExecution()           // Debugger.resume + virtual time advance
  -> OnExecutionResumed()
  -> ExecuteAction()             // Calls user-provided ActionCallback
  -> [action calls OnActionDispatched()]
  -> WaitUntil()                 // Handles wait_until from params
  -> OnWaitUntilComplete()
  -> PauseExecution()            // Virtual time pause + Debugger.pause
  -> OnExecutionPaused()
  -> CaptureScreenshot()         // Page is frozen
  -> SendResponse()              // Include timing + virtual_time info
```

### Phase 2: ExecutionController (Per-Tab State)

**New file: `chrome/browser/abp/abp_execution_controller.h`**

Manages debugger + virtual time state per tab:

```cpp
class AbpExecutionController {
 public:
  struct State {
    bool debugger_enabled = false;
    bool virtual_time_enabled = false;
    bool paused = true;  // Start paused
    double virtual_time_base_ticks_ms = 0;
  };

  // Enable execution control for a tab (one-time setup)
  void Enable(const std::string& tab_id,
              AbpCdpClient* client,
              std::optional<double> initial_virtual_time,
              base::OnceClosure then);

  // Resume before action
  void Resume(const std::string& tab_id,
              AbpCdpClient* client,
              base::OnceClosure then);

  // Pause after action
  void Pause(const std::string& tab_id,
             AbpCdpClient* client,
             base::OnceClosure then);

  // Get current state
  const State* GetState(const std::string& tab_id) const;

  // Cleanup on tab close
  void OnTabClosed(const std::string& tab_id);

 private:
  std::map<std::string, State> states_;
};
```

**Resume implementation:**
```cpp
void AbpExecutionController::Resume(tab_id, client, then) {
  auto* state = GetOrCreateState(tab_id);
  if (!state->paused) {
    std::move(then).Run();
    return;
  }

  // 1. Debugger.resume
  client->SendCommand("Debugger.resume", {},
    BindOnce(&OnDebuggerResumed, tab_id, client, std::move(then)));
}

void OnDebuggerResumed(tab_id, client, then, success, result) {
  // 2. setVirtualTimePolicy("advance")
  base::Value::Dict params;
  params.Set("policy", "advance");
  client->SendCommand("Emulation.setVirtualTimePolicy", params,
    BindOnce(&OnResumeComplete, tab_id, std::move(then)));
}

void OnResumeComplete(tab_id, then, ...) {
  states_[tab_id].paused = false;
  std::move(then).Run();
}
```

**Pause implementation:**
```cpp
void AbpExecutionController::Pause(tab_id, client, then) {
  auto* state = GetOrCreateState(tab_id);
  if (state->paused) {
    std::move(then).Run();
    return;
  }

  // 1. setVirtualTimePolicy("pause") - freeze time first
  base::Value::Dict params;
  params.Set("policy", "pause");
  client->SendCommand("Emulation.setVirtualTimePolicy", params,
    BindOnce(&OnVirtualTimePaused, tab_id, client, std::move(then)));
}

void OnVirtualTimePaused(tab_id, client, then, ...) {
  // 2. Debugger.pause - halt JS
  client->SendCommand("Debugger.pause", {},
    BindOnce(&OnPauseComplete, tab_id, std::move(then)));
}

void OnPauseComplete(tab_id, then, ...) {
  states_[tab_id].paused = true;
  std::move(then).Run();
}
```

### Phase 3: Action Integration

**Refactor existing actions to use AbpActionContext:**

Before (current Click implementation - ~100 lines):
```cpp
void AbpController::Click(tab_id, params, callback) {
  // Manual validation, screenshot, CDP calls, response...
}
```

After (with AbpActionContext - ~20 lines):
```cpp
void AbpController::Click(tab_id, params, callback) {
  AbpActionContext::Run(
      this, tab_id, params,
      // Action implementation - just the core logic
      base::BindOnce([](AbpActionContext* ctx) {
        double x = ctx->params().FindDouble("x").value_or(0);
        double y = ctx->params().FindDouble("y").value_or(0);

        base::Value::Dict click_params;
        click_params.Set("type", "mousePressed");
        click_params.Set("x", x);
        click_params.Set("y", y);
        click_params.Set("button", "left");
        click_params.Set("clickCount", 1);

        ctx->client()->SendCommand("Input.dispatchMouseEvent", click_params,
          base::BindOnce([](AbpActionContext* ctx, bool ok, const std::string&) {
            // Mouse release...
            ctx->OnActionDispatched();
          }, base::RetainedRef(ctx)));
      }),
      std::move(callback));
}
```

**Actions to refactor:**
- `Click()` - mouse click
- `Type()` - text input
- `Navigate()` - URL navigation
- `Reload()` - page reload
- `GoBack()` / `GoForward()` - history navigation
- `Execute()` - JavaScript execution (if it modifies state)

**Read-only actions (no context needed):**
- `ListTabs()` - just reads state
- `GetTab()` - just reads state
- `Screenshot()` - standalone screenshot without action

### Phase 4: wait_until Integration

Wait behavior per wait_until type:

| Type | Behavior |
|------|----------|
| `immediate` | Resume -> dispatch -> pause immediately |
| `action_complete` | Resume -> dispatch -> wait for quiescence -> pause |
| `network_idle` | Resume -> dispatch -> wait for network + idle_time -> pause |
| `time` | Resume -> dispatch -> real wall-clock delay (duration_ms) -> pause |

**Note**: `time` wait uses real time, not virtual time. This allows the page to execute JS for a real duration before pausing. Virtual time continues to advance during this real delay.

### Phase 5: Configuration

**Add command-line flag:**
```cpp
// abp_switches.h
const char kAbpVirtualTime[] = "abp-virtual-time";
```

**Add to action request params (optional override):**
```json
{
  "x": 100, "y": 200,
  "virtual_time": {
    "resume_before": true,
    "pause_after": true
  }
}
```

**Add to response:**
```json
{
  "result": {"status": "clicked"},
  "virtual_time": {
    "at_start": 1700000000.0,
    "at_end": 1700000001.2,
    "paused": true
  }
}
```

## CDP Commands Used

**Debugger domain (JS execution control):**
- `Debugger.enable` - Required once before pause/resume works
- `Debugger.pause` - Halt JavaScript execution
- `Debugger.resume` - Resume JavaScript execution
- `Debugger.paused` event - Fired when execution halts

**Emulation domain (virtual time):**
- `Emulation.setVirtualTimePolicy` with:
  - `policy`: "pause" | "advance" | "pauseIfNetworkFetchesPending"
  - `initialVirtualTime`: Unix epoch seconds (optional)
  - Returns: `virtualTimeTicksBase` in milliseconds
- `Emulation.virtualTimeBudgetExpired` event (for budget-based advancement)

## Edge Cases

1. **Tab closure during action**: Clean up execution_states_ entry
2. **Multiple tabs**: Each tab has independent execution state
3. **Navigation**: Debugger state may reset - need to re-enable after navigation
4. **DevTools disconnect**: Re-enable debugger + virtual time on reconnect
5. **Debugger.resume when not paused**: Check state before calling resume
6. **Nested frames**: Debugger.pause affects all frames in the tab
7. **Service workers**: May need separate handling (different target)

## Verification

1. Enable ABP with `--enable-abp --abp-execution-control`
2. Navigate to a page with:
   ```javascript
   setInterval(() => console.log('tick', Date.now()), 100);
   document.body.onclick = () => console.log('clicked', Date.now());
   ```
3. Call click action
4. Verify:
   - Console shows "clicked" during action
   - Timer ticks stop after action completes
   - No new console output while paused
5. Call next action
6. Verify:
   - Timer ticks resume
   - Date.now() values are consistent (virtual time advanced)

## Files Summary

| File | Changes |
|------|---------|
| `chrome/browser/abp/abp_action_context.h` | **NEW** - AbpActionContext class definition |
| `chrome/browser/abp/abp_action_context.cc` | **NEW** - Action flow implementation (resume/wait/pause/screenshot) |
| `chrome/browser/abp/abp_execution_controller.h` | **NEW** - AbpExecutionController class definition |
| `chrome/browser/abp/abp_execution_controller.cc` | **NEW** - Debugger + virtual time state management |
| `chrome/browser/abp/abp_controller.h` | Add AbpExecutionController member |
| `chrome/browser/abp/abp_controller.cc` | Refactor Click/Type/Navigate/etc to use AbpActionContext |
| `chrome/browser/abp/abp_switches.h` | Add --abp-execution-control flag |
| `chrome/browser/abp/abp_switches.cc` | Define flag |
| `chrome/browser/abp/BUILD.gn` | Add new source files |
| `plans/API.md` | Document /tabs/{id}/execution endpoint |
