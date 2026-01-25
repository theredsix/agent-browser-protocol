#!/bin/bash
# Test script for ABP MCP Server

set -e

ABP_URL=${ABP_URL:-http://localhost:8222}
MCP_URL="$ABP_URL/mcp"

echo "Testing ABP MCP Server at $MCP_URL"
echo "========================================"

# Test 1: Initialize session
echo -e "\n1. Testing MCP Initialize..."
INIT_RESPONSE=$(curl -s -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json, text/event-stream" \
  -d '{
    "jsonrpc": "2.0",
    "id": 1,
    "method": "initialize",
    "params": {
      "protocolVersion": "2025-03-26",
      "clientInfo": {"name": "test-client", "version": "1.0"},
      "capabilities": {}
    }
  }')
echo "Response: $INIT_RESPONSE"

# Extract session ID from response
SESSION_ID=$(echo "$INIT_RESPONSE" | jq -r '._mcpSessionId // empty')
if [ -n "$SESSION_ID" ]; then
  echo "Session ID: $SESSION_ID"
else
  echo "Warning: No session ID in response"
fi

# Test 2: Send initialized notification
echo -e "\n2. Testing notifications/initialized..."
curl -s -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json, text/event-stream" \
  ${SESSION_ID:+-H "Mcp-Session-Id: $SESSION_ID"} \
  -d '{
    "jsonrpc": "2.0",
    "method": "notifications/initialized"
  }' && echo "(202 Accepted expected)"

# Test 3: List tools
echo -e "\n3. Testing tools/list..."
TOOLS_RESPONSE=$(curl -s -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json, text/event-stream" \
  ${SESSION_ID:+-H "Mcp-Session-Id: $SESSION_ID"} \
  -d '{
    "jsonrpc": "2.0",
    "id": 2,
    "method": "tools/list",
    "params": {}
  }')
echo "Available tools:"
echo "$TOOLS_RESPONSE" | jq -r '.result.tools[]?.name' 2>/dev/null || echo "$TOOLS_RESPONSE"

# Test 4: Call browser_get_status
echo -e "\n4. Testing tools/call browser_get_status..."
STATUS_RESPONSE=$(curl -s -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json, text/event-stream" \
  ${SESSION_ID:+-H "Mcp-Session-Id: $SESSION_ID"} \
  -d '{
    "jsonrpc": "2.0",
    "id": 3,
    "method": "tools/call",
    "params": {
      "name": "browser_get_status",
      "arguments": {}
    }
  }')
echo "Browser status: $STATUS_RESPONSE"

# Test 5: Call browser_list_tabs
echo -e "\n5. Testing tools/call browser_list_tabs..."
TABS_RESPONSE=$(curl -s -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json, text/event-stream" \
  ${SESSION_ID:+-H "Mcp-Session-Id: $SESSION_ID"} \
  -d '{
    "jsonrpc": "2.0",
    "id": 4,
    "method": "tools/call",
    "params": {
      "name": "browser_list_tabs",
      "arguments": {}
    }
  }')
echo "Tabs: $TABS_RESPONSE"

# Test 6: Ping
echo -e "\n6. Testing ping..."
PING_RESPONSE=$(curl -s -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json, text/event-stream" \
  ${SESSION_ID:+-H "Mcp-Session-Id: $SESSION_ID"} \
  -d '{
    "jsonrpc": "2.0",
    "id": 5,
    "method": "ping",
    "params": {}
  }')
echo "Ping response: $PING_RESPONSE"

# Test 7: Invalid method
echo -e "\n7. Testing invalid method (should return error)..."
ERROR_RESPONSE=$(curl -s -X POST "$MCP_URL" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json, text/event-stream" \
  ${SESSION_ID:+-H "Mcp-Session-Id: $SESSION_ID"} \
  -d '{
    "jsonrpc": "2.0",
    "id": 6,
    "method": "invalid/method",
    "params": {}
  }')
echo "Error response: $ERROR_RESPONSE"

# Test 8: DELETE session (optional)
if [ -n "$SESSION_ID" ]; then
  echo -e "\n8. Testing DELETE session..."
  curl -s -X DELETE "$MCP_URL" \
    -H "Mcp-Session-Id: $SESSION_ID" && echo "(204 No Content expected)"
fi

echo -e "\n========================================"
echo "MCP Server tests completed!"
