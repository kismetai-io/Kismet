// Startup: the page must come up, tell the editor it is ready, and survive the
// bridge arriving late or never — the editor binds it asynchronously.
import { test, expect } from '../support/panel.js';

test('boots, shows the plugin version and tells the editor it is ready', async ({ panel }) => {
	await panel.open({ html: { version: '9.8.7' } });
	await expect(panel.page.locator('.intro-version')).toHaveText('9.8.7');
	expect(await panel.callCount('notifyuiready')).toBe(1);
	// The startup hydration the editor relies on.
	const names = (await panel.calls()).map(c => c.name);
	for (const name of ['loadtabstate', 'listsessions', 'getqueuejson', 'autocheckforupdate']) expect(names).toContain(name);
});

test('waits for a bridge that is bound after the page loads', async ({ panel }) => {
	await panel.open({ bridge: 'late', bridgeDelayMs: 600 });
	expect(await panel.callCount('notifyuiready')).toBe(1);
});

test('renders without a bridge and never reports ready', async ({ panel }) => {
	await panel.open({ bridge: 'absent' });
	await expect(panel.page.locator('.intro-screen')).toBeVisible();
	await panel.page.waitForTimeout(300);
	expect(await panel.page.evaluate(() => typeof window.ue)).toBe('undefined');
});

test('restores the saved tab layout from the editor', async ({ panel }) => {
	await panel.open({
		responses: {
			loadtabstate: JSON.stringify({ tabs: [{ id: 't1', sessionId: 's1', title: 'Door logic' }, { id: 't2', sessionId: 's2', title: 'Inventory' }], activeTabId: 't2' }),
			loadsession: '[]',
		},
	});
	await expect(panel.page.locator('#tab-bar')).toContainText('Door logic');
	await expect(panel.page.locator('#tab-bar')).toContainText('Inventory');
});

test('renders markdown in CEF, which lacks Array.prototype.at', async ({ panel }) => {
	// UE's embedded CEF rejects .at despite reporting Chromium 128; marked needs it.
	await panel.page.addInitScript(() => { delete Array.prototype.at; delete String.prototype.at; });
	await panel.open();
	await panel.emitAll([['text_start', {}], ['text_delta', { text: 'a **bold** word' }], ['text_end', {}]]);
	await expect(panel.page.locator('#chat .msg strong')).toHaveText('bold');
});

test.describe('sign-in and consent state', () => {
	test('a signed-in user with an unacknowledged telemetry notice sees it once', async ({ panel }) => {
		await panel.open();
		await panel.initialState({ hasSession: true, consentKnown: true, consented: false });
		const notice = panel.page.locator('#login-state-telemetry');
		await expect(notice).toBeVisible();
		await notice.getByRole('button').first().click();
		expect(await panel.callCount('accepttelemetryconsent')).toBe(1);
		await expect(notice).toBeHidden();
	});

	test('an unconfirmed consent value never blocks the chat', async ({ panel }) => {
		await panel.open();
		await panel.initialState({ hasSession: true, consentKnown: false, consented: false });
		await expect(panel.page.locator('#login-state-telemetry')).toBeHidden();
		await expect(panel.page.locator('#input')).toBeVisible();
	});

	for (const [shape, value] of [['a primitive', false], ['{ReturnValue}', { ReturnValue: false }], ['an array', [false]], ['a string', 'false']]) {
		test(`a missing API key reported as ${shape} opens Settings instead of sending`, async ({ panel }) => {
			// UE may resolve bool UFUNCTIONs as wrappers; `!!{ReturnValue:false}` is true.
			await panel.open({ responses: { hasapikey: value } });
			await panel.initialState();
			await panel.send('make a door');
			await expect.poll(() => panel.callCount('opensettings')).toBe(1);
			expect(await panel.callCount('sendmessagewithrefs')).toBe(0);
		});
	}
});
