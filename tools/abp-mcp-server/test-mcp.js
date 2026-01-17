#!/usr/bin/env node
/**
 * Simple MCP server test script
 * Sends JSON-RPC messages and prints responses
 */

import { spawn } from 'child_process';
import { createInterface } from 'readline';

const server = spawn('node', ['dist/index.js'], {
  stdio: ['pipe', 'pipe', 'inherit'],
  env: { ...process.env, ABP_URL: 'http://localhost:9222' }
});

let messageId = 1;

function send(method, params = {}) {
  const msg = {
    jsonrpc: '2.0',
    id: messageId++,
    method,
    params
  };
  const json = JSON.stringify(msg);
  server.stdin.write(`Content-Length: ${Buffer.byteLength(json)}\r\n\r\n${json}`);
  console.log(`\n>>> Sent: ${method}`);
}

// Parse responses
let buffer = '';
server.stdout.on('data', (chunk) => {
  buffer += chunk.toString();

  while (true) {
    const headerEnd = buffer.indexOf('\r\n\r\n');
    if (headerEnd === -1) break;

    const header = buffer.slice(0, headerEnd);
    const lengthMatch = header.match(/Content-Length: (\d+)/);
    if (!lengthMatch) break;

    const length = parseInt(lengthMatch[1]);
    const bodyStart = headerEnd + 4;

    if (buffer.length < bodyStart + length) break;

    const body = buffer.slice(bodyStart, bodyStart + length);
    buffer = buffer.slice(bodyStart + length);

    try {
      const response = JSON.parse(body);
      if (response.result) {
        // Truncate large base64 data for display
        const display = JSON.stringify(response.result, (key, value) => {
          if (key === 'data' && typeof value === 'string' && value.length > 100) {
            return value.slice(0, 50) + '...[truncated]';
          }
          return value;
        }, 2);
        console.log(`<<< Response:\n${display}`);
      } else if (response.error) {
        console.log(`<<< Error: ${JSON.stringify(response.error, null, 2)}`);
      }
    } catch (e) {
      console.log(`<<< Parse error: ${e.message}`);
    }
  }
});

// Run tests
async function runTests() {
  console.log('=== MCP Server Test ===\n');

  // Initialize
  send('initialize', {
    protocolVersion: '2024-11-05',
    capabilities: {},
    clientInfo: { name: 'test-client', version: '1.0.0' }
  });
  await sleep(500);

  // List tools
  send('tools/list', {});
  await sleep(500);

  // List tabs
  send('tools/call', { name: 'browser_list_tabs', arguments: {} });
  await sleep(500);

  // Get tab ID from first response and navigate
  send('tools/call', {
    name: 'browser_navigate',
    arguments: {
      tab_id: '67139AE79B98362E942F5768E672F5EC',
      url: 'https://example.com'
    }
  });
  await sleep(2000);

  // Take screenshot with markup
  send('tools/call', {
    name: 'browser_screenshot',
    arguments: {
      tab_id: '67139AE79B98362E942F5768E672F5EC',
      markup: 'interactive'
    }
  });
  await sleep(2000);

  // Execute JavaScript
  send('tools/call', {
    name: 'browser_execute_javascript',
    arguments: {
      tab_id: '67139AE79B98362E942F5768E672F5EC',
      expression: 'document.title'
    }
  });
  await sleep(500);

  console.log('\n=== Tests Complete ===');
  process.exit(0);
}

function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

runTests();
