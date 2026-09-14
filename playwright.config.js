const { defineConfig, devices } = require('@playwright/test');

const e2eRunner = process.env.MTSCOPE_E2E_RUNNER || '';
const e2eBinary = process.env.MTSCOPE_E2E_BINARY || './build/release/meshat-monitor';

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
    command: `rm -f playwright-test.db && ${e2eRunner} ${e2eBinary} --database ./playwright-test.db --host 127.0.0.1 --http-port 18099 --web-root ./web`,
    url: 'http://127.0.0.1:18099',
    reuseExistingServer: false,
    timeout: 15_000,
  },
  projects: [
    { name: 'chromium', use: { ...devices['Desktop Chrome'] } },
    { name: 'mobile', use: { ...devices['Pixel 5'] } },
  ],
});
