#!/usr/bin/env node
/**
 * Direct test of ABP MCP server functionality
 * Tests the tool handlers directly without MCP protocol overhead
 */

// Simulate the server's abpRequest function
const ABP_URL = 'http://localhost:9222';

async function abpRequest(method, path, body) {
  const url = `${ABP_URL}${path}`;
  const response = await fetch(url, {
    method,
    headers: { 'Content-Type': 'application/json' },
    body: body ? JSON.stringify(body) : undefined,
  });
  const text = await response.text();
  return text ? JSON.parse(text) : {};
}

async function test(name, fn) {
  try {
    console.log(`\n=== ${name} ===`);
    const result = await fn();
    const display = JSON.stringify(result, (key, value) => {
      if (key === 'data' && typeof value === 'string' && value.length > 100) {
        return value.slice(0, 50) + `...[${value.length} chars total]`;
      }
      return value;
    }, 2);
    console.log(display);
    return result;
  } catch (error) {
    console.error(`Error: ${error.message}`);
    return null;
  }
}

async function runTests() {
  console.log('ABP MCP Server - Direct Function Tests');
  console.log('======================================');

  // Test 1: List tabs
  const tabs = await test('browser_list_tabs', () =>
    abpRequest('GET', '/api/v1/tabs')
  );

  if (!tabs || !tabs.length) {
    console.error('No tabs found!');
    process.exit(1);
  }

  const tabId = tabs[0].id;
  console.log(`\nUsing tab ID: ${tabId}`);

  // Test 2: Get tab info
  await test('browser_get_tab_info', () =>
    abpRequest('GET', `/api/v1/tabs/${tabId}`)
  );

  // Test 3: Navigate
  await test('browser_navigate', () =>
    abpRequest('POST', `/api/v1/tabs/${tabId}/navigate`, {
      url: 'https://example.com'
    })
  );

  // Wait for navigation
  await new Promise(r => setTimeout(r, 2000));

  // Test 4: Get tab info after navigation
  await test('browser_get_tab_info (after nav)', () =>
    abpRequest('GET', `/api/v1/tabs/${tabId}`)
  );

  // Test 5: Screenshot without markup
  await test('browser_screenshot (no markup)', () =>
    abpRequest('POST', `/api/v1/tabs/${tabId}/screenshot`, {
      screenshot: { format: 'webp', quality: 50, markup: 'none' }
    })
  );

  // Test 6: Screenshot with markup
  await test('browser_screenshot (interactive markup)', () =>
    abpRequest('POST', `/api/v1/tabs/${tabId}/screenshot`, {
      screenshot: { format: 'webp', quality: 50, markup: 'interactive' }
    })
  );

  // Test 7: Execute JavaScript
  await test('browser_execute_javascript', () =>
    abpRequest('POST', `/api/v1/tabs/${tabId}/execute`, {
      script: 'document.title'
    })
  );

  // Test 8: Click (on the "More information" link area)
  await test('browser_click', () =>
    abpRequest('POST', `/api/v1/tabs/${tabId}/click`, {
      x: 400, y: 200
    })
  );

  // Wait for potential navigation
  await new Promise(r => setTimeout(r, 1000));

  // Test 9: Type
  await test('browser_type', () =>
    abpRequest('POST', `/api/v1/tabs/${tabId}/type`, {
      text: 'Hello MCP!'
    })
  );

  // Test 10: Create new tab
  const newTab = await test('browser_new_tab', () =>
    abpRequest('POST', '/api/v1/tabs', { url: 'https://httpbin.org/html' })
  );

  // Test 11: List tabs again
  await test('browser_list_tabs (after new tab)', () =>
    abpRequest('GET', '/api/v1/tabs')
  );

  // Test 12: Close the new tab
  if (newTab && newTab.id) {
    await test('browser_close_tab', () =>
      abpRequest('DELETE', `/api/v1/tabs/${newTab.id}`)
    );
  }

  console.log('\n======================================');
  console.log('All tests completed!');
}

runTests().catch(console.error);
