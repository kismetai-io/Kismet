// Tool-call cards: a result must land on the card of the call that produced it.
// Providers reuse step numbers and tool_use_ids across turns, so matching is by
// call_id first, then tool_use_id, and only by step for old payloads with neither.
import { test, expect } from '../support/panel.js';

test.beforeEach(async ({ panel }) => {
	await panel.open();
	await panel.initialState();
	await panel.editor('onAgentStarted');
});

const icon = card => card.locator('.tool-icon');

test('a call shows a pending card that its result completes', async ({ panel }) => {
	await panel.emit('tool_call', { step: 1, tool: 'read_blueprint', call_id: 'r/c0', args: { path: '/Game/BP_Door' } });
	const card = panel.toolCards().first();
	await expect(icon(card)).toHaveClass(/pending/);
	await panel.emit('tool_result', { step: 1, call_id: 'r/c0', is_error: false, result: { success: true } });
	await expect(card).toHaveAttribute('data-completed', 'true');
	await expect(icon(card)).toHaveClass(/success/);
});

test('a card expands to show its input and result', async ({ panel }) => {
	await panel.emit('tool_call', { step: 1, tool: 'read_blueprint', call_id: 'r/c0', args: { path: '/Game/BP_Door' } });
	await panel.emit('tool_result', { step: 1, call_id: 'r/c0', is_error: false, result: { success: true, data: { nodes: 3 } } });
	const card = panel.toolCards().first();
	await panel.expandToolCard(card);
	await expect(card.locator('.tool-detail')).toContainText('"/Game/BP_Door"');
	await expect(card.locator('.tool-detail')).toContainText('"nodes": 3');
});

test('a typed failure marks the card failed even when the payload says success', async ({ panel }) => {
	await panel.emit('tool_call', { step: 1, tool: 'edit_blueprint', tool_use_id: 'A' });
	await panel.emit('tool_result', { step: 1, tool_use_id: 'A', is_error: true, result: { success: true, data: { operations_failed: 1 } } });
	await expect(icon(panel.toolCards().first())).toHaveClass(/error/);
	await expect(panel.page.locator('#chat .tool-group-failure')).toContainText('edit_blueprint: failed');
});

test('a typed success wins over a payload that reports failure', async ({ panel }) => {
	// The payload is user-owned data (e.g. a Blueprint string field), not a verdict.
	await panel.emit('tool_call', { step: 1, tool: 'read_blueprint' });
	await panel.emit('tool_result', { step: 1, is_error: false, result: { success: false, error: 'user-owned payload' } });
	await expect(icon(panel.toolCards().first())).toHaveClass(/success/);
});

test('legacy untyped results are judged from their envelope', async ({ panel }) => {
	await panel.emit('tool_call', { step: 1, tool: 'read_blueprint' });
	await panel.emit('tool_result', { step: 1, result: '<tool_use_error>{"success":false}</tool_use_error>' });
	await panel.emit('tool_call', { step: 2, tool: 'read_blueprint' });
	await panel.emit('tool_result', { step: 2, result: '{"success":false}' });
	await expect(icon(panel.toolCards().nth(0))).toHaveClass(/error/);
	await expect(icon(panel.toolCards().nth(1))).toHaveClass(/error/);
});

test('call_id wins over a reused step and tool_use_id', async ({ panel }) => {
	await panel.emit('tool_call', { step: 1, tool: 'read_blueprint', call_id: 'old/c0', tool_use_id: 'same' });
	await panel.emit('tool_call', { step: 1, tool: 'read_blueprint', call_id: 'new/c0', tool_use_id: 'same' });
	await panel.emit('tool_result', { step: 1, call_id: 'old/c0', tool_use_id: 'same', is_error: false, result: { success: true } });
	await expect(panel.toolCards().nth(0)).toHaveAttribute('data-completed', 'true');
	await expect(panel.toolCards().nth(1)).not.toHaveAttribute('data-completed', 'true');
});

test('an unknown call_id never falls back to matching by step', async ({ panel }) => {
	await panel.emit('tool_call', { step: 1, tool: 'read_blueprint', call_id: 'r/c0', tool_use_id: 'A' });
	await panel.emit('tool_result', { step: 1, call_id: 'missing', tool_use_id: 'A', is_error: false, result: { success: true } });
	await expect(panel.toolCards().first()).not.toHaveAttribute('data-completed', 'true');
	await expect(icon(panel.toolCards().first())).toHaveClass(/pending/);
});

test('tool_use_id picks the right card when calls share a step', async ({ panel }) => {
	await panel.emit('tool_call', { step: 1, tool: 'read_blueprint', tool_use_id: 'A' });
	await panel.emit('tool_call', { step: 1, tool: 'read_blueprint', tool_use_id: 'B' });
	await panel.emit('tool_result', { step: 1, tool_use_id: 'B', is_error: false, result: { success: true } });
	await expect(panel.toolCards().nth(0)).not.toHaveAttribute('data-completed', 'true');
	await expect(panel.toolCards().nth(1)).toHaveAttribute('data-completed', 'true');
});

test('a legacy step-only result still completes its card', async ({ panel }) => {
	await panel.emit('tool_call', { step: 1, tool: 'read_blueprint' });
	await panel.emit('tool_result', { step: 1, result: { success: true } });
	await expect(panel.toolCards().first()).toHaveAttribute('data-completed', 'true');
	await expect(icon(panel.toolCards().first())).toHaveClass(/success/);
});

test('the group summary counts reads and failures', async ({ panel }) => {
	await panel.emit('tool_call', { step: 1, tool: 'read_blueprint', call_id: 'c1' });
	await panel.emit('tool_result', { step: 1, call_id: 'c1', is_error: false, result: { success: true } });
	await panel.emit('tool_call', { step: 2, tool: 'edit_blueprint', call_id: 'c2' });
	await panel.emit('tool_result', { step: 2, call_id: 'c2', is_error: true, result: { success: false } });
	await expect(panel.page.locator('#chat .tool-group-meta').first()).toContainText('1 failed');
});

test('stopping a run leaves no card spinning', async ({ panel }) => {
	// KNOWN BUG: onAgentStopped does not settle in-flight cards, so a call the
	// run never answered spins forever. Delete test.fail() with the fix (or
	// delete this test if the runtime is guaranteed to send every tool_result).
	test.fail(true, 'in-flight tool cards keep spinning after the run stops');
	await panel.emit('tool_call', { step: 1, tool: 'read_blueprint', call_id: 'c1' });
	await panel.emit('done', { completion_summary: { status: 'cancelled', summary: 'Stopped.', packages: [] } });
	await panel.editor('onAgentStopped');
	await expect(panel.page.locator('#chat .tool-call .spinner')).toHaveCount(0);
});
