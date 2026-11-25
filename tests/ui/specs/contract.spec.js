// The page and the editor talk through two name lists that no compiler checks:
// bridge methods (page -> C++) and window.on* callbacks (C++ -> page). A rename
// on one side only fails at runtime, inside the editor. These tests read both
// sides from source and fail on any mismatch.
import vm from 'node:vm';
import { test, expect } from '@playwright/test';
import {
	bridgeCallsInPage, bridgeMethods, callbacksInvokedByEditor, pageCallbacks, readUiHtml, servedUiHtml,
} from '../support/contract.js';

test('every bridge method the page calls is a UFUNCTION of UKismetAIBridge', () => {
	const exposed = new Set(bridgeMethods().map(m => m.name));
	expect(exposed.size, 'KismetAIBridge.h parsed to no UFUNCTIONs').toBeGreaterThan(50);
	const missing = [...bridgeCallsInPage()].filter(n => !exposed.has(n)).sort();
	expect(missing, 'ui.html calls bridge methods KismetAIBridge.h does not declare').toEqual([]);
});

test('every callback the editor invokes is defined by the page', () => {
	const defined = pageCallbacks();
	const missing = [...callbacksInvokedByEditor()].filter(n => !defined.has(n)).sort();
	expect(missing, 'C++ calls window callbacks ui.html never defines').toEqual([]);
});

test('every callback the page defines is invoked by the editor', () => {
	const invoked = callbacksInvokedByEditor();
	const unused = [...pageCallbacks()].filter(n => !invoked.has(n)).sort();
	expect(unused, 'ui.html defines callbacks no C++ code calls (dead, or the C++ side was renamed)').toEqual([]);
});

test('the served page has no unsubstituted placeholders', () => {
	expect(servedUiHtml().match(/\{\{[A-Z_]+\}\}/g)).toBeNull();
});

test('every inline script block parses', () => {
	const scripts = [...readUiHtml().matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/g)].map(m => m[1]).filter(s => s.trim());
	expect(scripts.length).toBeGreaterThanOrEqual(5);
	scripts.forEach((source, i) => {
		expect(() => new vm.Script(source, { filename: `ui.html script block ${i + 1}` })).not.toThrow();
	});
});

test('the page loads nothing from outside itself', () => {
	const html = readUiHtml();
	expect(html.match(/<(?:script|link|img|iframe)\b[^>]*\b(?:src|href)\s*=\s*["']https?:/gi)).toBeNull();
	expect(html.match(/@import\s+url\(\s*["']?https?:/gi)).toBeNull();
});

const staticMarkup = () => readUiHtml().replace(/<script\b[^>]*>[\s\S]*?<\/script>/g, '').replace(/<style\b[^>]*>[\s\S]*?<\/style>/g, '');

test('element ids in the static markup are unique', () => {
	const ids = [...staticMarkup().matchAll(/\sid="([^"]+)"/g)].map(m => m[1]);
	const dupes = [...new Set(ids.filter((id, i) => ids.indexOf(id) !== i))].sort();
	expect(dupes).toEqual([]);
});

test('every static id the script looks up exists in the markup', () => {
	const html = readUiHtml();
	const markupIds = new Set([...html.matchAll(/\sid="([^"]+)"/g)].map(m => m[1]));
	// Ids also appear in markup the script builds; only flag ids that occur nowhere else.
	const looked = [...new Set([...html.matchAll(/getElementById\('([A-Za-z0-9_-]+)'\)/g)].map(m => m[1]))];
	const missing = looked.filter(id => !markupIds.has(id) && !html.includes(`id = '${id}'`) && !html.includes(`.id='${id}'`)
		&& !new RegExp(`\\.id\\s*=\\s*['"]${id}['"]`).test(html) && !new RegExp(`id=\\\\?["']${id}\\\\?["']`).test(html));
	expect(missing.sort(), 'getElementById targets with no element anywhere in ui.html').toEqual([]);
});
