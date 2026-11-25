// What a finished run tells the user. The terminal summary comes from the
// runtime ledger, not from model prose, so it must say exactly what the editor
// recorded: which assets changed, compiled, saved, or are in an unknown state.
import { test, expect } from '../support/panel.js';

test.beforeEach(async ({ panel }) => {
	await panel.open();
	await panel.initialState();
	await panel.editor('onAgentStarted');
});

const STATUSES = [
	['complete', 'Run finished', 'done'],
	['partial', 'Partial', 'done'],
	['cancelled', 'Cancelled', 'done'],
	['failed', 'Failed', 'error'],
	['uncertain', 'Uncertain', 'done'],
	['waiting_for_user', 'Waiting for you', 'done'],
];

for (const [status, title, event] of STATUSES) {
	test(`a ${status} run shows its ledger evidence, copyable`, async ({ panel }) => {
		await panel.emit(event, {
			completion_summary: {
				status, summary: 'Recorded evidence.',
				packages: [{ package: '/Game/A', summary: 'changed; compile failed; save failed' }, { package: '/Game/B', summary: 'changed; compiled; saved' }],
				unknown_operations: ['edit: outcome unknown'],
			},
		});
		const notice = panel.notices().last();
		await expect(panel.notices()).toHaveCount(1);
		await expect(notice.locator('.outcome-title')).toHaveText(title);
		if (status === 'complete') await expect(notice).toHaveClass(/outcome-complete/);
		else await expect(notice).not.toHaveClass(/outcome-complete/);
		const copied = await panel.copyText(notice);
		expect(copied).toBe(`${title}\nRecorded evidence.\n/Game/A: changed; compile failed; save failed\n/Game/B: changed; compiled; saved\nedit: outcome unknown`);
	});
}

test('an unknown status is shown as Uncertain, never as success', async ({ panel }) => {
	await panel.emit('done', { completion_summary: { status: 'something_new', summary: 'x', packages: [] } });
	await expect(panel.notices().last().locator('.outcome-title')).toHaveText('Uncertain');
});

test('a complete run that changed an asset says the changes were verified', async ({ panel }) => {
	await panel.emit('done', { completion_summary: { status: 'complete', summary: '1 asset changed.', packages: [{ package: '/Game/A', mutation_count: 1, summary: 'saved; compile passed' }] } });
	await expect(panel.notices().last().locator('.outcome-title')).toHaveText('Changes verified');
});

test('asset names and summaries are shown as text, not markup', async ({ panel }) => {
	await panel.emit('done', { completion_summary: { status: 'uncertain', summary: 'Unknown mutation state.', packages: [{ package: '<img src=x onerror="window.__pwned=1">', summary: '<script>bad()</script>' }] } });
	const notice = panel.notices().last();
	await expect(notice).toContainText('<img src=x');
	await expect(notice).toContainText('<script>bad()</script>');
	await expect(notice.locator('img, script')).toHaveCount(0);
	expect(await panel.page.evaluate(() => window.__pwned)).toBeUndefined();
});

test('a legacy notice that announces a following summary waits for it', async ({ panel }) => {
	await panel.emit('mutation_outcomes', { terminal_summary_follows: true, needs_attention: true, unsaved_packages: ['/Game/A'] });
	await expect(panel.notices()).toHaveCount(0);
	await panel.emit('done', { completion_summary: { status: 'partial', summary: 'Unresolved evidence.', packages: [{ package: '/Game/A', summary: 'Not saved.' }] } });
	await expect(panel.notices()).toHaveCount(1);
	await expect(panel.notices().last()).toContainText('/Game/A: Not saved.');
});

test('a completion summary takes precedence over a legacy payload in the same event', async ({ panel }) => {
	await panel.emit('done', { completion_summary: { status: 'waiting_for_user', summary: 'Waiting for your answer.', packages: [] }, mutation_outcomes: { needs_attention: true, unsaved_packages: ['/Game/Legacy'] } });
	await expect(panel.notices()).toHaveCount(1);
	await expect(panel.notices().last()).not.toContainText('Legacy');
});

test('a legacy terminal payload explains each unsafe asset state', async ({ panel }) => {
	await panel.emit('done', {
		mutation_outcomes: {
			needs_attention: true,
			unsaved_packages: ['/Game/Unsaved'],
			compile_failed_packages: ['/Game/Broken'],
			unrestored_packages: ['/Game/Half'],
			packages: [{ package: '/Game/Unsaved', save_error: 'read-only file' }, { package: '/Game/Broken', save_status: 'saved' }],
		},
	});
	const notice = panel.notices().last();
	await expect(notice.locator('.outcome-title')).toHaveText('Check these assets');
	await expect(notice).toContainText('Not saved: /Game/Unsaved');
	await expect(notice).toContainText('(read-only file)');
	await expect(notice).toContainText('Does not compile: /Game/Broken — this uncompilable state was saved to disk.');
	await expect(notice).toContainText('Rollback incomplete: /Game/Half');
});

test('an interrupted edit reports the reconciliation verdict per asset', async ({ panel }) => {
	await panel.emit('mutation_reconciliation', {
		tool_name: 'edit_blueprint', verdict: 'changed_since_intent', replay_blocked: true,
		assets: [{ asset: '/Game/BP_Door', verdict: 'not_applied', differences: ['Nodes: 4 -> 4'] }],
	});
	const notice = panel.notices().last();
	await expect(notice).toContainText('Interrupted edit checked');
	await expect(notice).toContainText('edit_blueprint was interrupted before its outcome was recorded: the asset changed since just before the call');
	await expect(notice).toContainText('/Game/BP_Door: no visible effect');
	await expect(notice).toContainText('Kismet will not repeat this exact call.');
});

test.describe('checkpoint reports after Reject or Reapply', () => {
	test('a clean full restore stays quiet', async ({ panel }) => {
		await panel.editor('onCheckpointReport', [{ asset: '/Game/A', restored: true, explanation: 'ok' }]);
		await expect(panel.notices()).toHaveCount(0);
	});

	test('a partial restore names what was not restored and where it stands', async ({ panel }) => {
		await panel.editor('onCheckpointReport', [
			{ asset: '/Game/A', restored: true, explanation: 'ok' },
			{ asset: '/Game/B', restored: false, status: 'disk_differs', explanation: 'file changed outside Kismet', memory_state: 'restored', disk_state: 'modified', evidence: ['hash mismatch'] },
		]);
		const notice = panel.notices().last();
		await expect(notice).toContainText('Not everything was restored');
		await expect(notice).toContainText('/Game/B: disk_differs — file changed outside Kismet (editor: restored; file: modified)');
		await expect(notice).toContainText('hash mismatch');
	});

	test('a restore that also reverted earlier unsaved edits is reported', async ({ panel }) => {
		await panel.editor('onCheckpointReport', [{ asset: '/Game/A', restored: true, explanation: 'ok', includes_previously_unsaved_edits: true }]);
		await expect(panel.notices().last()).toContainText('Changes restored');
	});
});
