// Assistant text: streaming, markdown, code blocks, asset links, and the rule
// that model output is untrusted. The panel holds the bridge — setapikey,
// installupdate, openinbrowser — so script in a reply is script in the editor.
import { test, expect } from '../support/panel.js';

async function reply(panel, ...chunks) {
	await panel.emit('text_start', {});
	for (const text of chunks) await panel.emit('text_delta', { text });
	await panel.emit('text_end', {});
	return panel.page.locator('#chat .msg-assistant, #chat .msg').last();
}

test.beforeEach(async ({ panel }) => {
	await panel.open();
	await panel.initialState();
});

test('streamed deltas join into one rendered message', async ({ panel }) => {
	const msg = await reply(panel, '## Plan\n', '1. Add a **Box** collision\n', '2. Bind `OnComponentBeginOverlap`');
	await expect(msg.locator('h2')).toHaveText('Plan');
	await expect(msg.locator('li')).toHaveCount(2);
	await expect(msg.locator('code')).toHaveText('OnComponentBeginOverlap');
	await expect(msg.locator('.streaming-cursor')).toHaveCount(0);
});

test('a fenced code block gets a language header and a working Copy button', async ({ panel }) => {
	const msg = await reply(panel, '```cpp\nUE_LOG(LogTemp, Log, TEXT("hi"));\n```');
	await expect(msg.locator('.code-header')).toHaveText('cpp');
	await msg.locator('pre').hover();
	await msg.locator('.copy-btn').click();
	await expect.poll(() => panel.calls('setclipboard')).toEqual([['UE_LOG(LogTemp, Log, TEXT("hi"));\n']]);
});

test('code blocks are syntax highlighted', async ({ panel }) => {
	// KNOWN BUG: marked v5 removed the `highlight` option, so the bundled
	// highlight.js never runs. Remove test.fail() with the fix.
	test.fail(true, 'marked.setOptions({highlight}) is ignored by marked v15');
	const msg = await reply(panel, '```cpp\nint32 Count = 0;\n```');
	await expect(msg.locator('code [class^="hljs-"]').first()).toBeVisible();
});

test('asset paths in prose open the asset; paths in code stay literal', async ({ panel }) => {
	await panel.respond('openasset', true);
	const msg = await reply(panel, 'Open /Game/Blueprints/BP_Door and compare `/Game/Literal/Path`.');
	await msg.getByText('/Game/Blueprints/BP_Door').click();
	await expect.poll(() => panel.calls('openasset')).toEqual([['/Game/Blueprints/BP_Door']]);
	await expect(msg.locator('code')).toHaveText('/Game/Literal/Path');
});

test.describe('model output is untrusted', () => {
	// KNOWN BUG: renderMarkdown passes marked.parse() output, raw HTML included,
	// to innerHTML with no sanitizer. Remove test.fail() once it is fixed; the
	// suite then fails until the marker goes, so the fix cannot regress quietly.
	const payloads = [
		['an inline event handler', '<img src=x onerror="window.__pwned=1">'],
		['a javascript: link', '[click](javascript:window.__pwned=1)'],
		['an svg onload', '<svg onload="window.__pwned=1"></svg>'],
	];
	for (const [name, payload] of payloads) {
		test(`${name} in a reply does not run`, async ({ panel }) => {
			test.fail(true, 'renderMarkdown does not sanitize HTML (see specs/messages.spec.js)');
			const msg = await reply(panel, 'Here you go: ' + payload);
			const link = msg.locator('a[href^="javascript:"]');
			if (await link.count()) await link.click();
			await panel.page.waitForTimeout(100);
			expect(await panel.page.evaluate(() => window.__pwned)).toBeUndefined();
			await expect(msg.locator('img[onerror], svg[onload], a[href^="javascript:"]')).toHaveCount(0);
		});
	}

	test('a user message is shown as text', async ({ panel }) => {
		await panel.respond('hasapikey', true);
		await panel.send('<img src=x onerror="window.__pwned=1"> fix it');
		await expect(panel.page.locator('#chat .msg-user').last()).toContainText('<img src=x');
		expect(await panel.page.evaluate(() => window.__pwned)).toBeUndefined();
	});
});
