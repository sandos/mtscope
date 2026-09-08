const { defineConfig, devices } = require('@playwright/test');

module.exports = defineConfig({
  testDir: './tests/e2e',
  fullyParallel: true,
  timeout: 15_000,
  reporter: 'list',
  use: {
    baseURL: 'http://127.0.0.1:18099',
    trace: 'on-first-retry',
  },
  webServer: {
    command: 'rm -f playwright-test.db && ./build/release/meshat-monitor --database ./playwright-test.db --host 127.0.0.1 --http-port 18099',
    url: 'http://127.0.0.1:18099',
    reuseExistingServer: false,
    timeout: 15_000,
  },
  projects: [
    { name: 'chromium', use: { ...devices['Desktop Chrome'] } },
    { name: 'mobile', use: { ...devices['Pixel 5'] } },
  ],
});
