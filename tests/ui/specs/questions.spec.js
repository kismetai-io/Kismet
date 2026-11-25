// ask_user: the agent pauses with a question card; the answer goes back to the
// editor through answerquestion(question_id, answers JSON) exactly once.
import { test, expect } from '../support/panel.js';

const QUESTION = {
	question_id: 'q-42',
	questions: [
		{ header: 'Door', question: 'Which door type?', options: [{ label: 'Sliding', description: 'Moves sideways' }, { label: 'Hinged' }] },
		{ question: 'Which sounds?', multiSelect: true, options: [{ label: 'Open' }, { label: 'Close' }, { label: 'Lock' }] },
	],
};

test.beforeEach(async ({ panel }) => {
	await panel.open();
	await panel.initialState();
	await panel.editor('onAgentStarted');
	await panel.emit('question', QUESTION);
});

const card = panel => panel.page.locator('#chat .question-card').last();
const answers = async panel => (await panel.calls('answerquestion')).map(([id, json]) => [id, JSON.parse(json)]);

test('shows each question with its header, options and descriptions', async ({ panel }) => {
	await expect(card(panel).locator('.question-header')).toHaveText('Door');
	await expect(card(panel).locator('.question-text')).toHaveText(['Which door type?', 'Which sounds?']);
	await expect(card(panel).getByRole('button', { name: /Sliding/ })).toContainText('Moves sideways');
});

test('single-choice keeps one selection; multi-choice keeps several', async ({ panel }) => {
	const c = card(panel);
	await c.getByRole('button', { name: /Sliding/ }).click();
	await c.getByRole('button', { name: /Hinged/ }).click();
	await c.getByRole('button', { name: 'Open' }).click();
	await c.getByRole('button', { name: 'Lock' }).click();
	await c.getByRole('button', { name: 'Send answer' }).click();
	expect(await answers(panel)).toEqual([['q-42', { 'Which door type?': 'Hinged', 'Which sounds?': ['Open', 'Lock'] }]]);
});

test('a typed answer is sent alongside or instead of the options', async ({ panel }) => {
	const c = card(panel);
	await c.locator('.question-other-input').first().fill('Revolving');
	await c.getByRole('button', { name: 'Close' }).click();
	await c.locator('.question-other-input').nth(1).fill('Creak');
	await c.getByRole('button', { name: 'Send answer' }).click();
	expect(await answers(panel)).toEqual([['q-42', { 'Which door type?': 'Revolving', 'Which sounds?': ['Close', 'Creak'] }]]);
});

test('an answered card locks so the answer cannot be sent twice', async ({ panel }) => {
	const c = card(panel);
	await c.getByRole('button', { name: /Sliding/ }).click();
	await c.getByRole('button', { name: 'Send answer' }).click();
	await expect(c).toHaveClass(/answered/);
	await expect(c.getByRole('button', { name: 'Answer sent' })).toBeDisabled();
	await expect(c.locator('.question-other-input').first()).toBeDisabled();
	expect(await panel.callCount('answerquestion')).toBe(1);
});

test('question text from the model is shown as text', async ({ panel }) => {
	await panel.emit('question', { question_id: 'q-x', questions: [{ question: '<img src=x onerror="window.__pwned=1">?', options: [{ label: '<b>yes</b>' }] }] });
	await expect(card(panel)).toContainText('<img src=x');
	await expect(card(panel).locator('img, b')).toHaveCount(0);
	expect(await panel.page.evaluate(() => window.__pwned)).toBeUndefined();
});

test('a malformed question event is ignored', async ({ panel }) => {
	const before = await panel.page.locator('#chat .question-card').count();
	await panel.emit('question', { question_id: 'q-bad', questions: [] });
	await panel.emit('question', { questions: [{ question: 'no id' }] });
	await expect(panel.page.locator('#chat .question-card')).toHaveCount(before);
});
