// Reopening a saved session rebuilds the chat from the journal the editor
// returns from loadsession. The replay must pair each tool result with its own
// call even when providers reuse ids, results arrive out of order, or a call
// never got a result — and it must render through the same code as a live run.
import { test, expect } from '../support/panel.js';

async function openSession(panel, entries) {
	await panel.respond('loadsession', JSON.stringify(entries));
	await panel.run('openSessionInTab', 'session-1', 'Saved chat');
	await expect.poll(() => panel.callCount('loadsession')).toBe(1);
	await expect.poll(() => panel.callCount('requestdiffstate')).toBeGreaterThan(0);
}

const use = (id, name = 'read_blueprint', input = {}) => ({ type: 'tool_use', id, name, input });
const result = (id, content, extra = {}) => ({ type: 'tool_result', tool_use_id: id, content, ...extra });
// 'unresolved' is any state that is neither a success tick nor a failure:
// a call with no recorded result must not be shown as having succeeded.
const cardStates = panel => panel.toolCards().evaluateAll(cards => cards.map(c => {
	const i = c.querySelector('.tool-icon');
	if (i.querySelector('.spinner')) return 'spinning';
	return i.classList.contains('error') ? 'error' : i.classList.contains('success') ? 'success' : 'unresolved';
}));
const knownReplayBug = () => test.fail(true,
	'resolveReplaySpinners paints a call with no recorded result as success; remove test.fail() with the fix');

test.beforeEach(async ({ panel }) => {
	await panel.open();
	await panel.initialState();
});

test('a session replays user text, assistant text and tool cards', async ({ panel }) => {
	await openSession(panel, [
		{ type: 'user_message', content: 'Add a door' },
		{ type: 'assistant_message', turn_id: 'r/t0', content: [{ type: 'text', text: 'Reading **BP_Door**.' }, use('A')] },
		{ type: 'tool_result_message', turn_id: 'r/t0', results: [result('A', 'ok', { is_error: false })] },
	]);
	await expect(panel.page.locator('#chat .msg-user')).toContainText('Add a door');
	await expect(panel.page.locator('#chat strong')).toHaveText('BP_Door');
	expect(await cardStates(panel)).toEqual(['success']);
});

test('a missing result does not shift the next turn onto the wrong card', async ({ panel }) => {
	knownReplayBug();
	await openSession(panel, [
		{ type: 'assistant_message', turn_id: 'r/t0', content: [use('A'), use('B')] },
		{ type: 'tool_result_message', turn_id: 'r/t0', call_id: 'r/t0/c0', results: [result('A', 'ok', { is_error: false })] },
		{ type: 'assistant_message', turn_id: 'r/t1', content: [use('C')] },
		{ type: 'tool_result_message', turn_id: 'r/t1', call_id: 'r/t1/c0', results: [result('C', 'bad', { is_error: true })] },
	]);
	expect(await cardStates(panel)).toEqual(['success', 'unresolved', 'error']);
});

test('legacy results out of order, mixed with notes, pair by tool_use_id', async ({ panel }) => {
	await openSession(panel, [
		{ type: 'assistant_message', content: [use('A'), use('B', 'edit_blueprint')] },
		{ type: 'tool_result_message', results: [{ type: 'text', text: 'note' }, result('B', '{"success":false}'), result('A', 'a')] },
	]);
	expect(await cardStates(panel)).toEqual(['success', 'error']);
});

test('a result for an unknown id does not complete any card', async ({ panel }) => {
	knownReplayBug();
	await openSession(panel, [
		{ type: 'assistant_message', content: [use('A')] },
		{ type: 'tool_result_message', results: [result('missing', 'bad')] },
	]);
	expect(await cardStates(panel)).toEqual(['unresolved']);
});

test('turn identity separates provider ids reused across turns', async ({ panel }) => {
	await openSession(panel, [
		{ type: 'assistant_message', turn_id: 'r/t0', content: [use('same')] },
		{ type: 'assistant_message', turn_id: 'r/t1', content: [use('same', 'edit_blueprint')] },
		{ type: 'tool_result_message', turn_id: 'r/t0', results: [result('same', 'old', { is_error: false })] },
		{ type: 'tool_result_message', turn_id: 'r/t1', results: [result('same', 'new', { is_error: true })] },
	]);
	expect(await cardStates(panel)).toEqual(['success', 'error']);
});

test('an explicit succeeded status overrides payload text', async ({ panel }) => {
	await openSession(panel, [
		{ type: 'assistant_message', turn_id: 'r/t0', content: [use('A')] },
		{ type: 'tool_result_message', turn_id: 'r/t0', status: 'succeeded', results: [result('A', '{"success":false}')] },
	]);
	expect(await cardStates(panel)).toEqual(['success']);
});

test('identity-free legacy results pair in order', async ({ panel }) => {
	await openSession(panel, [
		{ type: 'assistant_message', content: [{ type: 'tool_use', name: 'read_blueprint' }, { type: 'tool_use', name: 'edit_blueprint' }] },
		{ type: 'tool_result_message', results: [{ type: 'tool_result', content: 'first' }, { type: 'tool_result', content: '{"success":false}' }] },
	]);
	expect(await cardStates(panel)).toEqual(['success', 'error']);
});

test('a missing result still pairs the next turn with its own call', async ({ panel }) => {
	// The pairing half of the case above, which passes today.
	await openSession(panel, [
		{ type: 'assistant_message', turn_id: 'r/t0', content: [use('A'), use('B')] },
		{ type: 'tool_result_message', turn_id: 'r/t0', results: [result('A', 'ok', { is_error: false })] },
		{ type: 'assistant_message', turn_id: 'r/t1', content: [use('C', 'edit_blueprint')] },
		{ type: 'tool_result_message', turn_id: 'r/t1', results: [result('C', 'bad', { is_error: true })] },
	]);
	const states = await cardStates(panel);
	expect(states[0]).toBe('success');
	expect(states[2]).toBe('error');
	expect(states).not.toContain('spinning');
});

test('terminal summaries replay through the live renderer', async ({ panel }) => {
	await openSession(panel, [
		{ type: 'done', completion_summary: { status: 'complete', summary: 'No editing operations.', packages: [] } },
		{ type: 'error', completion_summary: { status: 'failed', summary: 'No changes completed.', packages: [] } },
	]);
	await expect(panel.notices().locator('.outcome-title')).toHaveText(['Run finished', 'Failed']);
});

test('legacy terminal outcomes stay visible on replay', async ({ panel }) => {
	await openSession(panel, [{ type: 'done', mutation_outcomes: { needs_attention: true, unsaved_packages: ['/Game/Legacy'] } }]);
	await expect(panel.notices().last()).toContainText('Not saved: /Game/Legacy');
});

test('a corrupt journal opens an empty chat instead of breaking the panel', async ({ panel }) => {
	await panel.respond('loadsession', '{not json');
	await panel.run('openSessionInTab', 'session-1', 'Broken');
	await expect.poll(() => panel.callCount('requestdiffstate')).toBeGreaterThan(0);
	await expect(panel.page.locator('#input')).toBeVisible();
});

test('a replayed session matches the same run seen live', async ({ panel, page }) => {
	// Live: the same tool traffic through onAgentEvent.
	await panel.editor('onAgentStarted');
	await panel.emitAll([
		['tool_call', { step: 1, tool: 'read_blueprint', tool_use_id: 'A' }],
		['tool_result', { step: 1, tool_use_id: 'A', is_error: false, result: 'ok' }],
		['tool_call', { step: 2, tool: 'edit_blueprint', tool_use_id: 'B' }],
		['tool_result', { step: 2, tool_use_id: 'B', is_error: true, result: 'bad' }],
		['done', { completion_summary: { status: 'partial', summary: 'One edit failed.', packages: [] } }],
	]);
	await panel.editor('onAgentStopped');
	const live = { cards: await cardStates(panel), titles: await panel.notices().locator('.outcome-title').allTextContents() };

	await openSession(panel, [
		{ type: 'assistant_message', turn_id: 't0', content: [use('A')] },
		{ type: 'tool_result_message', turn_id: 't0', results: [result('A', 'ok', { is_error: false })] },
		{ type: 'assistant_message', turn_id: 't1', content: [use('B', 'edit_blueprint')] },
		{ type: 'tool_result_message', turn_id: 't1', results: [result('B', 'bad', { is_error: true })] },
		{ type: 'done', completion_summary: { status: 'partial', summary: 'One edit failed.', packages: [] } },
	]);
	expect({ cards: await cardStates(panel), titles: await panel.notices().locator('.outcome-title').allTextContents() }).toEqual(live);
});
