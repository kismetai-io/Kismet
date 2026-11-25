// The composer: what reaches the model, what runs locally as a slash command,
// and what happens while a run is in progress. Driven by real key presses and
// clicks; assertions are on the bridge calls the editor receives.
import { test, expect } from '../support/panel.js';

const COMMANDS = [
	['/provider  lmstudio  meta/muse-glimmer  http://127.0.0.1:8888', 'setprovider', ['lmstudio', 'meta/muse-glimmer', 'http://127.0.0.1:8888']],
	['/model sonnet', 'setmodel', ['sonnet']],
	['/effort high', 'seteffortlevel', ['high']],
	['/fastmode off', 'setfastmode', ['off']],
];
const MESSAGES = ['make a door', '/Game/BP_Door has a bug', '/providers'];

test.beforeEach(async ({ panel }) => {
	await panel.open({ responses: { hasapikey: true } });
	await panel.initialState();
	await panel.clearCalls();
});

const input = panel => panel.page.locator('#input');
const sendButton = panel => panel.page.locator('#btn-send');

async function clickSend(panel, text) {
	await input(panel).fill(text);
	// Close the command dropdown as a user would. Only when open: Escape also stops a run.
	if (await panel.page.locator('#cmd-dropdown').isVisible()) await panel.page.keyboard.press('Escape');
	await sendButton(panel).click();
}

const sentToModel = async panel => (await panel.calls()).filter(c => /^sendmessage/.test(c.name));

test.describe('Enter', () => {
	for (const text of MESSAGES) {
		test(`sends "${text}" to the model`, async ({ panel }) => {
			await panel.send(text);
			await expect.poll(async () => (await sentToModel(panel)).map(c => c.args[0])).toEqual([text]);
			await expect(panel.page.locator('#chat .msg-user').last()).toContainText(text);
			await expect(input(panel)).toHaveValue('');
		});
	}

	for (const [text, method, args] of COMMANDS) {
		test(`runs ${text.split(' ')[0]} locally`, async ({ panel }) => {
			await panel.send(text);
			await expect.poll(() => panel.calls(method)).toEqual([args]);
			expect(await sentToModel(panel)).toEqual([]);
			await expect(input(panel)).toHaveValue('');
		});
	}

	test('Shift+Enter adds a line instead of sending', async ({ panel }) => {
		await input(panel).fill('first line');
		await input(panel).press('Shift+Enter');
		await input(panel).type('second line');
		await expect(input(panel)).toHaveValue('first line\nsecond line');
		expect(await sentToModel(panel)).toEqual([]);
	});

	test('an empty or blank composer sends nothing', async ({ panel }) => {
		await panel.send('   ');
		await panel.page.waitForTimeout(100);
		expect(await panel.calls()).toEqual([]);
	});
});

test.describe('Send button', () => {
	for (const text of MESSAGES) {
		test(`sends "${text}" to the model`, async ({ panel }) => {
			await clickSend(panel, text);
			await expect.poll(async () => (await sentToModel(panel)).map(c => c.args[0])).toEqual([text]);
		});
	}

	for (const [text, method, args] of COMMANDS) {
		test(`runs ${text.split(' ')[0]} locally`, async ({ panel }) => {
			// KNOWN BUG on main, fixed by kismetai-io/Kismet#6: the button sends
			// slash commands to the model as chat. Remove test.fail() when #6 lands.
			test.fail(true, 'Send button bypasses slash-command handling (kismetai-io/Kismet#6)');
			await clickSend(panel, text);
			await expect.poll(() => panel.calls(method), { timeout: 1000 }).toEqual([args]);
			expect(await sentToModel(panel)).toEqual([]);
		});
	}
});

test.describe('while a run is in progress', () => {
	test.beforeEach(async ({ panel }) => {
		await panel.send('first task');
		await expect.poll(async () => (await sentToModel(panel)).length).toBe(1);
		await panel.editor('onAgentStarted');
		await panel.clearCalls();
	});

	test('the Send button stops the run', async ({ panel }) => {
		await clickSend(panel, '/provider ollama x');
		await expect.poll(() => panel.callCount('abort')).toBe(1);
		expect(await sentToModel(panel)).toEqual([]);
	});

	test('Enter queues the message for after the run', async ({ panel }) => {
		await panel.send('then add a window');
		await expect.poll(async () => (await panel.calls('enqueuemessage')).map(a => a[0])).toEqual(['then add a window']);
		expect(await panel.callCount('steernow')).toBe(0);
	});

	test('Ctrl/Cmd+Enter steers the current run', async ({ panel }) => {
		await input(panel).fill('use a sliding door instead');
		await input(panel).press('ControlOrMeta+Enter');
		await expect.poll(async () => (await panel.calls('steernow')).map(a => a[0])).toEqual(['use a sliding door instead']);
		expect(await panel.callCount('enqueuemessage')).toBe(0);
	});

	test('Escape stops the run', async ({ panel }) => {
		await input(panel).press('Escape');
		await expect.poll(() => panel.callCount('abort')).toBe(1);
	});

	test('the editor queue state is shown', async ({ panel }) => {
		await panel.editor('onQueueState', [{ id: 'q1', text: 'then add a window' }]);
		await expect(panel.page.getByText('then add a window')).toBeVisible();
	});
});

test.describe('command dropdown', () => {
	test('typing / lists the commands and filters as you type', async ({ panel }) => {
		const dropdown = panel.page.locator('#cmd-dropdown');
		await input(panel).fill('/');
		await expect(dropdown).toBeVisible();
		await expect(dropdown).toContainText('/model');
		await input(panel).fill('/mod');
		await expect(dropdown).toContainText('/model');
		await expect(dropdown).not.toContainText('/init');
	});

	test('Escape closes it without running anything', async ({ panel }) => {
		await input(panel).fill('/');
		await input(panel).press('Escape');
		await expect(panel.page.locator('#cmd-dropdown')).toBeHidden();
		expect(await panel.calls()).toEqual([]);
	});

	test('Tab completes the highlighted command into the composer', async ({ panel }) => {
		await input(panel).fill('/effo');
		await input(panel).press('Tab');
		await expect(input(panel)).toHaveValue('/effort ');
	});
});
