const { test, expect } = require('@playwright/test');

test('shows the empty monitor dashboard', async ({ page, request }) => {
  await page.goto('/');

  await expect(page).toHaveTitle('Meshtastic Monitor');
  await expect(page.getByRole('heading', { name: 'Meshtastic Monitor' })).toBeVisible();
  await expect(page.locator('#connection-status')).toHaveClass(/connected/);
  await expect(page.getByText('0 logical packets · 0 seen nodes')).toBeVisible();
  await expect(page.getByRole('columnheader', { name: 'Details' })).toBeVisible();
  await expect(page.getByRole('columnheader', { name: 'Observer' })).toBeVisible();
  await expect(page.getByRole('tab', { name: 'Nodes' })).toBeVisible();
  await expect(page.getByLabel('Filter packets')).toBeVisible();
  const receivedHeader = page.getByRole('button', { name: /Received/ });
  await expect(receivedHeader).not.toHaveAttribute('data-sort-direction');
  await receivedHeader.click();
  await expect(receivedHeader).toHaveAttribute('data-sort-direction', 'ascending');
  await receivedHeader.click();
  await expect(receivedHeader).toHaveAttribute('data-sort-direction', 'descending');
  await receivedHeader.click();
  await expect(receivedHeader).not.toHaveAttribute('data-sort-direction');

  const response = await request.get('/api/packets');
  expect(response.ok()).toBeTruthy();
  await expect(response.json()).resolves.toEqual([]);

  const nodesResponse = await request.get('/api/nodes');
  expect(nodesResponse.ok()).toBeTruthy();
  await expect(nodesResponse.json()).resolves.toEqual([]);

  const statsResponse = await request.get('/api/stats');
  expect(statsResponse.ok()).toBeTruthy();
  await expect(statsResponse.json()).resolves.toMatchObject({
    database_bytes: expect.any(Number),
    logical_packets: 0,
    observations: 0,
    measurements: 0,
    known_nodes: 0,
    first_observed_at: null,
    last_observed_at: null,
    host_ram_used_bytes: expect.any(Number),
    host_ram_total_bytes: expect.any(Number),
    process_ram_bytes: expect.any(Number),
  });

  await page.getByRole('tab', { name: 'Nodes' }).click();
  await expect(page.getByRole('columnheader', { name: 'Node ID' })).toBeVisible();
  await expect(page.getByLabel('Filter nodes')).toBeVisible();
  await expect(page.getByText('No nodeinfo packets received yet')).toBeVisible();

  await page.getByRole('tab', { name: 'Stats' }).click();
  await expect(page.getByRole('heading', { name: 'Monitor', exact: true })).toBeVisible();
  await expect(page.getByRole('heading', { name: 'Host' })).toBeVisible();
  await expect(page.getByRole('heading', { name: 'Storage and data' })).toBeVisible();
  await expect(page.getByText('Database size')).toBeVisible();
  await expect(page.locator('#monitor-stats').getByText('CPU usage')).toBeVisible();
  await expect(page.locator('#host-stats').getByText('RAM usage')).toBeVisible();
  await expect(page.getByText('Logical packets', { exact: true })).toBeVisible();
  await expect(page.getByRole('heading', { name: 'API response times' })).toBeVisible();
  await expect(page.locator('#http-client-stats')).toContainText('receive timeouts');

  const logsResponse = await request.get('/api/logs');
  expect(logsResponse.ok()).toBeTruthy();
  await expect(logsResponse.json()).resolves.toEqual(expect.arrayContaining([expect.objectContaining({ source: 'HTTP' })]));
  await page.getByRole('tab', { name: 'Logs' }).click();
  await expect(page.locator('#logs')).toBeVisible();
});

test('keeps the dashboard usable on mobile', async ({ page }) => {
  await page.goto('/');

  await expect(page.getByRole('heading', { name: 'Meshtastic Monitor' })).toBeVisible();
  await expect(page.locator('#packets-panel table')).toBeVisible();
  const viewport = page.viewportSize();
  const bodyWidth = await page.locator('body').evaluate((element) => element.scrollWidth);
  expect(bodyWidth).toBeLessThanOrEqual(viewport.width);
});

test('supports keyboard navigation and filtering shortcuts', async ({ page }) => {
  await page.goto('/');

  await page.keyboard.press('2');
  await expect(page.locator('#nodes-panel')).toBeVisible();
  await page.keyboard.press('/');
  await expect(page.getByLabel('Filter nodes')).toBeFocused();

  await page.keyboard.type('sensor');
  await page.keyboard.press('Escape');
  await expect(page.getByLabel('Filter nodes')).toHaveValue('');

  await page.keyboard.press('1');
  await expect(page.locator('#packets-panel')).toBeVisible();

  await page.keyboard.press('3');
  await expect(page.locator('#stats-panel')).toBeVisible();
});

test('keeps node coordinate columns balanced when status is hidden', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('tab', { name: 'Nodes' }).click();

  const nodesTable = page.locator('#nodes-panel table');
  const latitudeColumn = nodesTable.locator('col.node-latitude-column');
  const longitudeColumn = nodesTable.locator('col.node-longitude-column');
  const statusColumn = nodesTable.locator('col.node-status-column');
  const widths = await Promise.all([latitudeColumn, longitudeColumn, statusColumn].map(column => column.evaluate(element => element.getBoundingClientRect().width)));

  expect(Math.abs(widths[0] - widths[1])).toBeLessThanOrEqual(1);
  expect(widths[2]).toBe(0);

  await page.getByLabel('Show node status').check();
  await expect(nodesTable.locator('th[data-column="status"]')).toBeVisible();
  await expect.poll(() => statusColumn.evaluate(element => element.getBoundingClientRect().width)).toBeGreaterThan(0);
});
