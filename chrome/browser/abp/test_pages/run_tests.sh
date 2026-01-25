#!/bin/bash
# ABP Integration Test Suite
# Tests core ABP functionality against test pages

# Don't use set -e as we want to continue after test failures

ABP_URL="http://localhost:8222"
HTTP_URL="http://localhost:8081"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

PASSED=0
FAILED=0

# Helper function to run a test
run_test() {
    local test_name="$1"
    local test_func="$2"

    echo -n "Testing $test_name... "

    if $test_func; then
        echo -e "${GREEN}PASSED${NC}"
        PASSED=$((PASSED + 1))
    else
        echo -e "${RED}FAILED${NC}"
        FAILED=$((FAILED + 1))
    fi
}

# Get the first tab ID
get_tab_id() {
    curl -s "$ABP_URL/api/v1/tabs" | jq -r '.[0].id'
}

# NAV-001: Navigate to a URL
test_nav_001() {
    local tab_id=$(get_tab_id)

    # Navigate to navigation test page
    curl -s -X POST "$ABP_URL/api/v1/tabs/$tab_id/navigate" \
        -H "Content-Type: application/json" \
        -d '{"url":"'"$HTTP_URL"'/navigation-test.html"}' > /dev/null

    sleep 1

    # Verify by checking URL
    local url=$(curl -s "$ABP_URL/api/v1/tabs/$tab_id" | jq -r '.url')
    [[ "$url" == *"navigation-test.html"* ]]
}

# CLICK-001: Click on a button
test_click_001() {
    local tab_id=$(get_tab_id)

    # Navigate to click test page
    curl -s -X POST "$ABP_URL/api/v1/tabs/$tab_id/navigate" \
        -H "Content-Type: application/json" \
        -d '{"url":"'"$HTTP_URL"'/click-input-test.html"}' > /dev/null

    sleep 1

    # Click the "Click Me" button at position (100, 100) - center of btn-fixed
    curl -s -X POST "$ABP_URL/api/v1/tabs/$tab_id/click" \
        -H "Content-Type: application/json" \
        -d '{"x":100,"y":100}' > /dev/null

    sleep 0.5

    # Check the click count
    local count=$(curl -s -X POST "$ABP_URL/api/v1/tabs/$tab_id/execute" \
        -H "Content-Type: application/json" \
        -d '{"script":"document.getElementById(\"click-count\").textContent"}' | jq -r '.result.value')

    [[ "$count" == "1" ]]
}

# TYPE-001: Type text into an input field
# NOTE: Uses simple string without "!" to avoid bash escaping issues
test_type_001() {
    local tab_id=$(get_tab_id)

    # Make sure we're on the click-input-test page
    curl -s -X POST "$ABP_URL/api/v1/tabs/$tab_id/navigate" \
        -H "Content-Type: application/json" \
        -d '{"url":"'"$HTTP_URL"'/click-input-test.html"}' > /dev/null

    sleep 1

    # Click on the text input to focus it (position 150, 195 - center of text-input)
    curl -s -X POST "$ABP_URL/api/v1/tabs/$tab_id/click" \
        -H "Content-Type: application/json" \
        -d '{"x":150,"y":195}' > /dev/null

    sleep 0.3

    # Type text (no "!" to avoid bash issues)
    curl -s -X POST "$ABP_URL/api/v1/tabs/$tab_id/type" \
        -H 'Content-Type: application/json' \
        -d '{"text":"Hello ABP"}' > /dev/null

    sleep 0.3

    # Check the input value
    local value=$(curl -s -X POST "$ABP_URL/api/v1/tabs/$tab_id/execute" \
        -H "Content-Type: application/json" \
        -d '{"script":"document.getElementById(\"text-input\").value"}' | jq -r '.result.value')

    [[ "$value" == "Hello ABP" ]]
}

# KEY-001: Press Enter to submit a form
test_key_001() {
    local tab_id=$(get_tab_id)

    # Navigate to click-input-test page
    curl -s -X POST "$ABP_URL/api/v1/tabs/$tab_id/navigate" \
        -H "Content-Type: application/json" \
        -d '{"url":"'"$HTTP_URL"'/click-input-test.html"}' > /dev/null

    sleep 1

    # Click on username field (center at 150, 495)
    curl -s -X POST "$ABP_URL/api/v1/tabs/$tab_id/click" \
        -H "Content-Type: application/json" \
        -d '{"x":150,"y":495}' > /dev/null

    sleep 0.3

    # Type username
    curl -s -X POST "$ABP_URL/api/v1/tabs/$tab_id/type" \
        -H 'Content-Type: application/json' \
        -d '{"text":"testuser"}' > /dev/null

    sleep 0.3

    # Press Enter to submit
    curl -s -X POST "$ABP_URL/api/v1/tabs/$tab_id/keyboard/press" \
        -H "Content-Type: application/json" \
        -d '{"key":"Enter"}' > /dev/null

    sleep 0.5

    # Check if form was submitted by checking typeof lastFormData (avoid bash ! issues)
    local result=$(curl -s -X POST "$ABP_URL/api/v1/tabs/$tab_id/execute" \
        -H "Content-Type: application/json" \
        -d '{"script":"typeof window.lastFormData"}' | jq -r '.result.value')

    [[ "$result" == "object" ]]
}

# EXEC-001: Execute JavaScript
test_exec_001() {
    local tab_id=$(get_tab_id)

    # Execute a simple calculation
    local result=$(curl -s -X POST "$ABP_URL/api/v1/tabs/$tab_id/execute" \
        -H "Content-Type: application/json" \
        -d '{"script":"2 + 2"}' | jq -r '.result.value')

    [[ "$result" == "4" ]]
}

# SCREENSHOT-001: Take a screenshot
test_screenshot_001() {
    local tab_id=$(get_tab_id)

    # Take a screenshot
    local response=$(curl -s -X POST "$ABP_URL/api/v1/tabs/$tab_id/screenshot" \
        -H "Content-Type: application/json" \
        -d '{"screenshot":{"format":"webp"}}')

    # Check that we got base64 data
    local data=$(echo "$response" | jq -r '.data // empty')
    [[ -n "$data" && ${#data} -gt 100 ]]
}

# ============================================
# VIRTUAL TIME TESTS
# These tests use a shared tab with execution control enabled.
# The tab is created once and reused across tests to avoid crashes
# when closing tabs with active debugger sessions.
# ============================================

# Global variable to hold the shared virtual time test tab
VTIME_TAB_ID=""

# Setup: Create a shared tab for virtual time tests
setup_vtime_tab() {
    # Create new tab
    local response=$(curl -s -X POST "$ABP_URL/api/v1/tabs" \
        -H "Content-Type: application/json" \
        -d '{"url":"'"$HTTP_URL"'/virtual-time-test.html"}')
    VTIME_TAB_ID=$(echo "$response" | jq -r '.id')

    sleep 2

    # Enable execution control (starts paused)
    curl -s -X POST "$ABP_URL/api/v1/tabs/$VTIME_TAB_ID/execution" \
        -H "Content-Type: application/json" \
        -d '{"paused": true}' > /dev/null

    sleep 1

    # Check if execution control is enabled
    local enabled=$(curl -s "$ABP_URL/api/v1/tabs/$VTIME_TAB_ID/execution" | jq -r '.enabled')
    if [[ "$enabled" = "true" ]]; then
        return 0
    else
        return 1
    fi
}

# Helper to reset page state between tests (reload and re-pause)
reset_vtime_tab() {
    # Resume execution first so navigation can complete
    curl -s -X POST "$ABP_URL/api/v1/tabs/$VTIME_TAB_ID/execution" \
        -H "Content-Type: application/json" \
        -d '{"paused": false}' > /dev/null

    sleep 0.3

    # Navigate to the test page again to reset state
    curl -s -X POST "$ABP_URL/api/v1/tabs/$VTIME_TAB_ID/navigate" \
        -H "Content-Type: application/json" \
        -d '{"url":"'"$HTTP_URL"'/virtual-time-test.html"}' > /dev/null

    sleep 1.5

    # Re-enable execution control (paused)
    curl -s -X POST "$ABP_URL/api/v1/tabs/$VTIME_TAB_ID/execution" \
        -H "Content-Type: application/json" \
        -d '{"paused": true}' > /dev/null

    sleep 0.5
}

# VTIME-001: Timer count frozen when paused
# Tests that setInterval does not increment when virtual time is paused
test_vtime_001() {
    # Check if we have a valid tab
    if [[ -z "$VTIME_TAB_ID" ]]; then
        return 1
    fi

    # Check if execution control is enabled
    local enabled=$(curl -s "$ABP_URL/api/v1/tabs/$VTIME_TAB_ID/execution" | jq -r '.enabled')
    if [[ "$enabled" = "true" ]]; then
        :  # continue
    else
        return 1
    fi

    # Get initial timer count
    local count1=$(curl -s -X POST "$ABP_URL/api/v1/tabs/$VTIME_TAB_ID/execute" \
        -H "Content-Type: application/json" \
        -d '{"script":"window.virtualTimeTest.getIntervalCount()"}' | jq -r '.result.value')

    # Wait 2 seconds (real wall-clock time)
    sleep 2

    # Get timer count again
    local count2=$(curl -s -X POST "$ABP_URL/api/v1/tabs/$VTIME_TAB_ID/execute" \
        -H "Content-Type: application/json" \
        -d '{"script":"window.virtualTimeTest.getIntervalCount()"}' | jq -r '.result.value')

    # If paused, counts should be equal (allow small delta for execute overhead)
    local diff=$((count2 - count1))
    [[ $diff -lt 3 ]]
}

# VTIME-002: Date.now() frozen when paused
# Tests that Date.now() returns same value when virtual time is paused
test_vtime_002() {
    # Reset the tab for this test
    reset_vtime_tab

    # Check if we have a valid tab
    if [[ -z "$VTIME_TAB_ID" ]]; then
        return 1
    fi

    # Check if execution control is enabled
    local enabled=$(curl -s "$ABP_URL/api/v1/tabs/$VTIME_TAB_ID/execution" | jq -r '.enabled')
    if [[ "$enabled" = "true" ]]; then
        :  # continue
    else
        return 1
    fi

    # Get Date.now() twice with a 1 second gap
    # When frozen, these should return identical values
    local time1=$(curl -s -X POST "$ABP_URL/api/v1/tabs/$VTIME_TAB_ID/execute" \
        -H "Content-Type: application/json" \
        -d '{"script":"Date.now()"}' | jq -r '.result.value')

    sleep 1

    local time2=$(curl -s -X POST "$ABP_URL/api/v1/tabs/$VTIME_TAB_ID/execute" \
        -H "Content-Type: application/json" \
        -d '{"script":"Date.now()"}' | jq -r '.result.value')

    # When virtual time is frozen, values should be identical
    # (comparing as strings to avoid large number arithmetic issues)
    [[ "$time1" = "$time2" ]]
}

# VTIME-003: setTimeout does not fire when paused
# Tests that scheduled timeouts don't execute while paused
test_vtime_003() {
    # Reset the tab for this test
    reset_vtime_tab

    # Check if we have a valid tab
    if [[ -z "$VTIME_TAB_ID" ]]; then
        return 1
    fi

    # Check if execution control is enabled
    local enabled=$(curl -s "$ABP_URL/api/v1/tabs/$VTIME_TAB_ID/execution" | jq -r '.enabled')
    if [[ "$enabled" = "true" ]]; then
        :  # continue
    else
        return 1
    fi

    # Schedule a timeout for 100ms
    curl -s -X POST "$ABP_URL/api/v1/tabs/$VTIME_TAB_ID/execute" \
        -H "Content-Type: application/json" \
        -d '{"script":"window.scheduleTestTimeout(100)"}' > /dev/null

    # Wait 500ms real time (timeout should have fired if running normally)
    sleep 0.5

    # Check if timeout fired
    local fired=$(curl -s -X POST "$ABP_URL/api/v1/tabs/$VTIME_TAB_ID/execute" \
        -H "Content-Type: application/json" \
        -d '{"script":"window.testTimeoutFired"}' | jq -r '.result.value')

    # If paused, timeout should NOT have fired
    [[ "$fired" = "false" ]]
}

# NAV-003: Navigate back and forward
test_nav_003() {
    local tab_id=$(get_tab_id)

    # Navigate to page 1
    curl -s -X POST "$ABP_URL/api/v1/tabs/$tab_id/navigate" \
        -H "Content-Type: application/json" \
        -d '{"url":"'"$HTTP_URL"'/navigation-test.html"}' > /dev/null

    sleep 1

    # Navigate to page 2
    curl -s -X POST "$ABP_URL/api/v1/tabs/$tab_id/navigate" \
        -H "Content-Type: application/json" \
        -d '{"url":"'"$HTTP_URL"'/navigation-test-page2.html"}' > /dev/null

    sleep 1

    # Verify we're on page 2
    local url=$(curl -s "$ABP_URL/api/v1/tabs/$tab_id" | jq -r '.url')
    if [[ "$url" != *"page2"* ]]; then
        return 1
    fi

    # Go back
    curl -s -X POST "$ABP_URL/api/v1/tabs/$tab_id/back" > /dev/null

    sleep 1

    # Verify we're back on page 1
    url=$(curl -s "$ABP_URL/api/v1/tabs/$tab_id" | jq -r '.url')
    if [[ "$url" != *"navigation-test.html"* ]]; then
        return 1
    fi

    # Go forward
    curl -s -X POST "$ABP_URL/api/v1/tabs/$tab_id/forward" > /dev/null

    sleep 1

    # Verify we're on page 2 again
    url=$(curl -s "$ABP_URL/api/v1/tabs/$tab_id" | jq -r '.url')
    [[ "$url" == *"page2"* ]]
}

# Main test execution
echo "================================"
echo "ABP Integration Test Suite"
echo "================================"
echo ""

# Check prerequisites
echo "Checking prerequisites..."
if ! curl -s "$ABP_URL/api/v1/tabs" > /dev/null 2>&1; then
    echo -e "${RED}ERROR: ABP server not running at $ABP_URL${NC}"
    exit 1
fi

if ! curl -s "$HTTP_URL/" > /dev/null 2>&1; then
    echo -e "${RED}ERROR: HTTP server not running at $HTTP_URL${NC}"
    exit 1
fi

echo -e "${GREEN}Prerequisites OK${NC}"
echo ""

# Run tests
echo "Running tests..."
echo ""

run_test "NAV-001 (Navigate to URL)" test_nav_001
run_test "CLICK-001 (Click button)" test_click_001
run_test "TYPE-001 (Type text)" test_type_001
run_test "KEY-001 (Press Enter for form submit)" test_key_001
run_test "EXEC-001 (Execute JavaScript)" test_exec_001
run_test "SCREENSHOT-001 (Take screenshot)" test_screenshot_001
run_test "NAV-003 (Back/Forward navigation)" test_nav_003

# Virtual Time Tests (if execution endpoint available)
echo ""
echo "--- Virtual Time Tests ---"
echo -n "Setting up virtual time test tab... "
if setup_vtime_tab; then
    echo -e "${GREEN}OK${NC}"
    run_test "VTIME-001 (Timer frozen when paused)" test_vtime_001
    run_test "VTIME-002 (Date.now frozen when paused)" test_vtime_002
    run_test "VTIME-003 (setTimeout not firing when paused)" test_vtime_003
else
    echo -e "${RED}FAILED${NC}"
    echo "Skipping virtual time tests - could not enable execution control"
    FAILED=$((FAILED + 3))
fi

# Summary
echo ""
echo "================================"
echo "Test Results"
echo "================================"
echo -e "Passed: ${GREEN}$PASSED${NC}"
echo -e "Failed: ${RED}$FAILED${NC}"
echo ""

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}Some tests failed.${NC}"
    exit 1
fi
