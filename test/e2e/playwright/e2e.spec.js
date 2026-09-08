const { test, expect } = require('@playwright/test');

const password = 'e2e-test-password';

async function register(page, prefix) {
  const username = `${prefix.slice(0, 3)}${Date.now().toString().slice(-8)}${Math.floor(Math.random() * 1000).toString().padStart(3, '0')}`;

  await page.goto('/');
  await page.getByRole('button', { name: 'Create an account' }).click();
  await page.locator('input[autocomplete="username"]').fill(username);
  await page.locator('input[autocomplete="new-password"]').fill(password);
  await page.getByRole('button', { name: 'Create account' }).click();
  await expect(page.getByRole('navigation', { name: 'Primary' })).toBeVisible();

  return username;
}

test.describe('Modern console E2E', () => {
  test('creates a spreadsheet and exposes collaboration controls', async ({ page }) => {
    await register(page, 'sheet');
    await page.getByRole('link', { name: 'Spreadsheets' }).click();
    await page.getByLabel('New spreadsheet name').fill('E2E spreadsheet');
    await page.getByRole('button', { name: 'Create', exact: true }).click();

    await expect(page.getByRole('heading', { name: 'E2E spreadsheet' })).toBeVisible();
    await page.getByRole('button', { name: 'Share' }).click();
    await expect(page.getByRole('region', { name: 'Spreadsheet sharing' })).toBeVisible();
    await expect(page.getByRole('button', { name: 'Create view link' })).toBeVisible();
  });

  test('creates and opens a workspace without converting its ID to a number', async ({ page }) => {
    await register(page, 'workspace');
    await page.getByRole('link', { name: 'Workspaces' }).click();
    await page.getByLabel('Workspace name').fill('E2E workspace');
    await page.getByRole('button', { name: 'Create workspace' }).click();

    const workspace = page.getByRole('link', { name: 'E2E workspace' });
    await expect(workspace).toBeVisible();
    await workspace.click();
    await expect(page.getByRole('heading', { name: 'E2E workspace' })).toBeVisible();
    await expect(page.getByRole('heading', { name: 'Members' })).toBeVisible();
  });

  test('creates a folder through the modern files page', async ({ page }) => {
    await register(page, 'files');
    await page.getByRole('link', { name: 'Files' }).click();
    await page.getByLabel('Folder name').fill('E2E folder');
    const [response] = await Promise.all([
      page.waitForResponse(candidate => candidate.url().includes('/api/v1/files/folder') && candidate.request().method() === 'POST'),
      page.getByRole('button', { name: 'Create folder' }).click()
    ]);
    expect((await response.json()).success).toBe(true);
    await expect(page.getByRole('heading', { name: /Folder: E2E folder/ })).toBeVisible();
  });

  test('renders points and mall routes from the modern navigation', async ({ page }) => {
    await register(page, 'navigation');
    await page.getByRole('link', { name: 'Points' }).click();
    await expect(page.getByRole('heading', { name: 'Points' })).toBeVisible();
    await page.getByRole('link', { name: 'Mall' }).click();
    await expect(page.getByRole('heading', { name: 'Mall' })).toBeVisible();
  });
});
