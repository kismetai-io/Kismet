// The `panel` fixture: the real ui.html in a real browser engine, wired to a fake
// editor. The fake bridge has exactly the UFUNCTIONs of KismetAIBridge.h, records
// every call, and answers with per-test responses. Editor-to-page traffic goes
// through the same window.on* callbacks SKismetAIPanel invokes.
//
// Every test also fails on an uncaught page error, an unexpected console.error,
// or any network request: the panel must work offline from one file.
import { test as base, expect } from '@playwright/test';
import { bridgeMethods, servedUiHtml } from './contract.js';

export const PANEL_URL = 'https://panel.kismet.test/ui.html';
const METHODS = bridgeMethods().map(({ name, defaultValue }) => ({ name, defaultValue }));

// Runs in the page before any of its scripts. Keep it self-contained.
function installFakeBridge({ methods, responses, mode, delayMs }) {
	const state = { calls: [], responses: { ...responses }, handlers: {} };
	window.__bridge = state;
	const api = {};
	for (const { name, defaultValue } of methods) {
		api[name] = function(...args) {
			state.calls.push({ name, args: JSON.parse(JSON.stringify(args)) });
			if (state.handlers[name]) return Promise.resolve().then(() => state.handlers[name](...args));
			let r = Object.prototype.hasOwnProperty.call(state.responses, name) ? state.responses[name] : defaultValue;
			if (Array.isArray(r) && r.__sequence) r = r.length > 1 ? r.shift() : r[0];
			if (r && typeof r === 'object' && r.__reject) return Promise.reject(new Error(r.__reject));
			return Promise.resolve(r);
		};
	}
	const attach = () => { window.ue = { kismetai: api }; };
	if (mode === 'immediate') attach();
	else if (mode === 'late') setTimeout(attach, delayMs);
}

export class Panel {
	constructor(page) {
		this.page = page;
		this.pageErrors = [];
		this.consoleErrors = [];
		this.externalRequests = [];
		this.allowedConsoleErrors = [];
		this.pendingResponses = {};
		page.on('pageerror', e => this.pageErrors.push(e.stack || String(e)));
		page.on('console', m => { if (m.type() === 'error') this.consoleErrors.push(m.text()); });
	}

	/** Queue a bridge response. Before open() it is present from the first call. */
	async respond(name, value) {
		this.#checkName(name);
		if (!this.opened) { this.pendingResponses[name] = value; return; }
		await this.page.evaluate(([n, v]) => { window.__bridge.responses[n] = v; }, [name, value]);
	}

	/** Successive calls get successive values; the last one repeats. */
	sequence(...values) { const a = [...values]; return Object.assign(a, { __sequence: true }); }

	/** A bridge call whose Promise rejects, as when the editor-side UFUNCTION is gone. */
	reject(message = 'bridge call failed') { return { __reject: message }; }

	/**
	 * Load the panel.
	 * @param {{bridge?: 'immediate'|'late'|'absent', bridgeDelayMs?: number, responses?: object, waitForReady?: boolean, html?: object}} opts
	 */
	async open({ bridge = 'immediate', bridgeDelayMs = 300, responses = {}, waitForReady = bridge !== 'absent', html = {} } = {}) {
		for (const name of Object.keys(responses)) this.#checkName(name);
		const body = servedUiHtml(html);
		await this.page.route('**/*', route => {
			const url = route.request().url();
			if (url === PANEL_URL) return route.fulfill({ status: 200, contentType: 'text/html; charset=utf-8', body });
			this.externalRequests.push(url);
			return route.abort();
		});
		await this.page.addInitScript(installFakeBridge, {
			methods: METHODS,
			responses: { ...this.pendingResponses, ...responses },
			mode: bridge,
			delayMs: bridgeDelayMs,
		});
		await this.page.goto(PANEL_URL);
		this.opened = true;
		if (waitForReady) await expect.poll(() => this.callCount('notifyuiready'), { message: 'panel never called notifyuiready' }).toBe(1);
	}

	/** Bridge calls recorded so far, optionally for one method. */
	async calls(name) {
		const all = await this.page.evaluate(() => window.__bridge ? window.__bridge.calls : []);
		return name ? all.filter(c => c.name === name).map(c => c.args) : all;
	}

	async callCount(name) { return (await this.calls(name)).length; }

	/** Forget recorded calls, e.g. the startup traffic. */
	async clearCalls() { await this.page.evaluate(() => { window.__bridge.calls.length = 0; }); }

	/** Invoke a window.on* callback the way SKismetAIPanel's ExecuteJavascript does. */
	async editor(callback, ...args) {
		await this.page.evaluate(([cb, a]) => {
			if (typeof window[cb] !== 'function') throw new Error('page has no ' + cb);
			return window[cb](...a);
		}, [callback, args]);
	}

	/** One agent event, as FKismetAIModule::BroadcastJS sends it. */
	emit(type, data = {}) { return this.editor('onAgentEvent', type, data); }

	async emitAll(events) { for (const [type, data] of events) await this.emit(type, data); }

	/** SKismetAIPanel::PushInitialState, with named instead of positional arguments. */
	initialState({
		blueprintPath = '', serverRunning = true, hasApiKey = true, model = 'Claude Sonnet',
		thinkingMode = 'adaptive', thinkingDisplay = 'Adaptive', thinkingBudget = 8000, effort = 'medium',
		hasSession = true, email = 'dev@example.test', consentKnown = true, consented = true,
	} = {}) {
		return this.editor('onInitialState', blueprintPath, serverRunning, hasApiKey, model, thinkingMode,
			thinkingDisplay, thinkingBudget, effort, hasSession, email, consentKnown, consented);
	}

	/** Call a page-level function by name, for logic with no event of its own. */
	async run(fn, ...args) {
		return this.page.evaluate(([f, a]) => {
			// Top-level function declarations in classic scripts are globals.
			const target = window[f] ?? (0, eval)(f);
			if (typeof target !== 'function') throw new Error('page has no function ' + f);
			return target(...a);
		}, [fn, args]);
	}

	/** Outcome and assistant notices in the active chat, newest last. */
	notices() { return this.page.locator('#chat .msg-outcome'); }

	/** Tool-call cards in the active chat, in order. */
	toolCards() { return this.page.locator('#chat .tool-call'); }

	/** Open the activity log and tool group around a card, then the card itself. */
	async expandToolCard(card) {
		// Collapse is CSS (max-height: 0), so go by the state classes, not visibility.
		const log = card.locator('xpath=ancestor::*[contains(concat(" ",@class," ")," activity-log ")][1]');
		if (await log.count() && /\bcollapsed\b/.test(await log.getAttribute('class'))) await log.locator('.activity-log-summary').first().click();
		const group = card.locator('xpath=ancestor::*[contains(concat(" ",@class," ")," tool-group ")][1]');
		if (!/\bexpanded\b/.test(await group.getAttribute('class'))) await group.locator('.tool-group-summary').first().click();
		await expect(group).toHaveClass(/\bexpanded\b/);
		await card.locator('.tool-summary').click();
		await expect(card.locator('.tool-detail')).toBeVisible();
	}

	/** Click a message's copy button and return what reached the editor clipboard. */
	async copyText(message) {
		const wrapper = message.locator('xpath=ancestor-or-self::*[contains(@class,"msg-wrapper")][1]');
		await wrapper.hover();
		const before = await this.callCount('setclipboard');
		await wrapper.locator('.msg-copy-btn').first().click();
		await expect.poll(() => this.callCount('setclipboard')).toBe(before + 1);
		return (await this.calls('setclipboard')).at(-1)[0];
	}

	allowConsoleError(pattern) { this.allowedConsoleErrors.push(pattern); }

	/** Type into the composer and press Enter, as a user would. */
	async send(text) {
		const input = this.page.locator('#input');
		await input.fill(text);
		await input.press('Enter');
	}

	assertClean() {
		const consoleErrors = this.consoleErrors.filter(t => !this.allowedConsoleErrors.some(p => p.test(t)));
		expect.soft(this.pageErrors, 'uncaught errors in the panel').toEqual([]);
		expect.soft(consoleErrors, 'console.error in the panel').toEqual([]);
		expect.soft(this.externalRequests, 'the panel must not load anything from the network').toEqual([]);
	}

	#checkName(name) {
		if (!METHODS.some(m => m.name === name)) throw new Error(`KismetAIBridge.h has no UFUNCTION named ${name}`);
	}
}

export const test = base.extend({
	panel: async ({ page }, use) => {
		const panel = new Panel(page);
		await use(panel);
		panel.assertClean();
	},
});

export { expect };
