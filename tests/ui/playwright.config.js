// Two engines because the panel runs in two: UE's embedded CEF (Chromium) on
// Windows and Linux, and WKWebView (WebKit) on macOS.
import { defineConfig, devices } from '@playwright/test';

export default defineConfig({
	testDir: './specs',
	fullyParallel: true,
	forbidOnly: !!process.env.CI,
	retries: 0,
	timeout: 15_000,
	expect: { timeout: 3_000 },
	reporter: process.env.CI ? [['list'], ['github'], ['junit', { outputFile: 'results/junit.xml' }]] : [['list']],
	outputDir: 'results/artifacts',
	use: {
		viewport: { width: 520, height: 900 },
		trace: 'retain-on-failure',
		screenshot: 'only-on-failure',
	},
	projects: [
		// Source-level checks of the page/editor contract; no browser.
		{ name: 'contract', testMatch: /contract\.spec\.js$/ },
		{ name: 'chromium', testIgnore: /contract\.spec\.js$/, use: { ...devices['Desktop Chrome'], viewport: { width: 520, height: 900 } } },
		{ name: 'webkit', testIgnore: /contract\.spec\.js$/, use: { ...devices['Desktop Safari'], viewport: { width: 520, height: 900 } } },
	],
});
