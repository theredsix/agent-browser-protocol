#!/usr/bin/env node
/**
 * Test the MCP server using the official MCP client SDK
 */

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

async function main() {
  console.log('=== MCP Client Test ===\n');

  // Create client transport connecting to our server
  const transport = new StdioClientTransport({
    command: 'node',
    args: ['dist/index.js'],
    env: { ...process.env, ABP_URL: 'http://localhost:9222' }
  });

  // Create MCP client
  const client = new Client(
    { name: 'test-client', version: '1.0.0' },
    { capabilities: {} }
  );

  try {
    // Connect to server
    console.log('Connecting to MCP server...');
    await client.connect(transport);
    console.log('Connected!\n');

    // List available tools
    console.log('--- Listing Tools ---');
    const tools = await client.listTools();
    console.log(`Found ${tools.tools.length} tools:`);
    tools.tools.forEach(t => console.log(`  - ${t.name}: ${t.description}`));

    // List resources
    console.log('\n--- Listing Resources ---');
    const resources = await client.listResources();
    console.log(`Found ${resources.resources.length} resources:`);
    resources.resources.forEach(r => console.log(`  - ${r.uri}: ${r.description}`));

    // Call browser_list_tabs
    console.log('\n--- Calling browser_list_tabs ---');
    const tabsResult = await client.callTool({ name: 'browser_list_tabs', arguments: {} });
    console.log('Result:', tabsResult.content[0].text);

    // Parse to get tab ID
    const tabs = JSON.parse(tabsResult.content[0].text);
    const tabId = tabs[0]?.id;

    if (tabId) {
      // Navigate to example.com
      console.log('\n--- Calling browser_navigate ---');
      const navResult = await client.callTool({
        name: 'browser_navigate',
        arguments: { tab_id: tabId, url: 'https://example.com' }
      });
      console.log('Result:', navResult.content[0].text);

      // Wait for page load
      await new Promise(r => setTimeout(r, 2000));

      // Take screenshot with markup
      console.log('\n--- Calling browser_screenshot (with markup) ---');
      const ssResult = await client.callTool({
        name: 'browser_screenshot',
        arguments: { tab_id: tabId, markup: 'interactive' }
      });
      const ssData = JSON.parse(ssResult.content[0].text);
      console.log(`Screenshot: ${ssData.format}, markup: ${ssData.markup}, data: ${ssData.data?.slice(0, 50)}...`);

      // Execute JavaScript
      console.log('\n--- Calling browser_execute_javascript ---');
      const jsResult = await client.callTool({
        name: 'browser_execute_javascript',
        arguments: { tab_id: tabId, expression: 'document.title' }
      });
      console.log('Result:', jsResult.content[0].text);

      // Read resource
      console.log('\n--- Reading browser://tabs resource ---');
      const tabsResource = await client.readResource({ uri: 'browser://tabs' });
      console.log('Resource:', tabsResource.contents[0].text);
    }

    console.log('\n=== All MCP Tests Passed! ===');
  } catch (error) {
    console.error('Error:', error);
    process.exit(1);
  } finally {
    await client.close();
  }
}

main();
