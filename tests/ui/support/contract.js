// Reads the two sides of the page/editor contract straight from the source:
// the UFUNCTIONs UKismetAIBridge exposes to the page, and the bridge methods
// and window.on* callbacks ui.html uses. The fake bridge is built from the
// header, so a test can never call a method the editor does not have.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
export const REPO_ROOT = path.resolve(here, '../../..');
export const PLUGIN_DIR = path.join(REPO_ROOT, 'KismetAI');
// KISMET_UI_HTML tests another copy of the page, e.g. one from a pull request.
export const UI_HTML = process.env.KISMET_UI_HTML ? path.resolve(process.env.KISMET_UI_HTML) : path.join(PLUGIN_DIR, 'Config', 'ui.html');
export const BRIDGE_HEADER = path.join(PLUGIN_DIR, 'Source', 'KismetAI', 'Public', 'KismetAIBridge.h');
export const SOURCE_DIR = path.join(PLUGIN_DIR, 'Source');

let cachedHtml;
export function readUiHtml() {
	cachedHtml ??= fs.readFileSync(UI_HTML, 'utf8');
	return cachedHtml;
}

/** The page as the editor serves it: SKismetAIPanel substitutes these placeholders. */
export function servedUiHtml({ version = '0.0.0-test', backendUrl = 'https://kismetai.test' } = {}) {
	return readUiHtml()
		.replaceAll('{{KISMETAI_VERSION}}', version)
		.replaceAll('{{KISMETAI_BACKEND_URL}}', backendUrl);
}

const DEFAULTS = { void: undefined, bool: false, int32: 0, FString: '' };

/**
 * Each UFUNCTION in KismetAIBridge.h as the page sees it. UE's WebBrowser binds
 * methods under their lowercased names and returns a Promise for every call.
 * @returns {{name: string, cppName: string, returnType: string, params: string[], defaultValue: unknown}[]}
 */
export function bridgeMethods() {
	const header = fs.readFileSync(BRIDGE_HEADER, 'utf8');
	const methods = [];
	const re = /UFUNCTION\([^)]*\)\s*\n\s*([A-Za-z0-9_]+)\s+([A-Za-z0-9_]+)\s*\(([^)]*)\)/g;
	for (const m of header.matchAll(re)) {
		const [, returnType, cppName, rawParams] = m;
		const params = rawParams.split(',').map(p => p.trim()).filter(Boolean)
			.map(p => p.split(/\s+/).pop().replace(/^&/, ''));
		methods.push({ name: cppName.toLowerCase(), cppName, returnType, params, defaultValue: DEFAULTS[returnType] });
	}
	return methods;
}

/** Bridge method names ui.html calls, e.g. `window.ue.kismetai.opensettings`. */
export function bridgeCallsInPage() {
	return new Set([...readUiHtml().matchAll(/window\.ue\.kismetai\.([A-Za-z0-9_]+)/g)].map(m => m[1]));
}

/** `window.onFoo = function` callbacks the page defines for the editor to call. */
export function pageCallbacks() {
	return new Set([...readUiHtml().matchAll(/window\.(on[A-Z][A-Za-z0-9]*)\s*=\s*(?:async\s+)?function/g)].map(m => m[1]));
}

/** `window.onFoo(` callbacks the C++ side invokes through ExecuteJavascript. */
export function callbacksInvokedByEditor() {
	const found = new Set();
	const walk = dir => {
		for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
			const full = path.join(dir, entry.name);
			if (entry.isDirectory()) { if (entry.name !== 'Tests') walk(full); continue; }
			if (!/\.(cpp|h)$/.test(entry.name)) continue;
			for (const m of fs.readFileSync(full, 'utf8').matchAll(/window\.(on[A-Z][A-Za-z0-9]*)\s*(?:&&|\()/g)) found.add(m[1]);
		}
	};
	walk(SOURCE_DIR);
	return found;
}
