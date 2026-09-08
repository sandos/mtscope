const { test, expect } = require('@playwright/test');

test('shows the empty monitor dashboard', async ({ page, request }) => {
  await page.goto('/');

  await expect(page).toHaveTitle('Meshtastic Monitor');
  await expect(page.getByRole('heading', { name: 'Meshtastic Monitor' })).toBeVisible();
  await expect(page.getByText('0 most recent packets')).toBeVisible();
  await expect(page.getByRole('columnheader', { name: 'Details' })).toBeVisible();

  const response = await request.get('/api/packets');
  expect(response.ok()).toBeTruthy();
  await expect(response.json()).resolves.toEqual([]);
});

test('keeps the dashboard usable on mobile', async ({ page }) => {
  await page.goto('/');

  await expect(page.getByRole('heading', { name: 'Meshtastic Monitor' })).toBeVisible();
  await expect(page.locator('table')).toBeVisible();
  const viewport = page.viewportSize();
  const bodyWidth = await page.locator('body').evaluate((element) => element.scrollWidth);
  expect(bodyWidth).toBeLessThanOrEqual(viewport.width);
});
