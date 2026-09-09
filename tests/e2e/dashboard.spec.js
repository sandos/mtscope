const { test, expect } = require('@playwright/test');

test('shows the empty monitor dashboard', async ({ page, request }) => {
  await page.goto('/');

  await expect(page).toHaveTitle('Meshtastic Monitor');
  await expect(page.getByRole('heading', { name: 'Meshtastic Monitor' })).toBeVisible();
  await expect(page.getByText('0 recent packets · 0 seen nodes')).toBeVisible();
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

  await page.getByRole('tab', { name: 'Nodes' }).click();
  await expect(page.getByRole('columnheader', { name: 'Node ID' })).toBeVisible();
  await expect(page.getByLabel('Filter nodes')).toBeVisible();
  await expect(page.getByText('No nodeinfo packets received yet')).toBeVisible();
});

test('keeps the dashboard usable on mobile', async ({ page }) => {
  await page.goto('/');

  await expect(page.getByRole('heading', { name: 'Meshtastic Monitor' })).toBeVisible();
  await expect(page.locator('#packets-panel table')).toBeVisible();
  const viewport = page.viewportSize();
  const bodyWidth = await page.locator('body').evaluate((element) => element.scrollWidth);
  expect(bodyWidth).toBeLessThanOrEqual(viewport.width);
});
