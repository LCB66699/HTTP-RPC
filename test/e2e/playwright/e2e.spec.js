const { test, expect } = require('@playwright/test');

const ADMIN_USER = 'e2eadmin_' + Date.now();
const ADMIN_PASS = 'admin1234';
const USER = 'e2euser_' + Date.now();
const PASS = 'test1234';

test.describe('HTTP-RPC E2E', () => {

  test.beforeAll(async ({ browser }) => {
    const ctx = await browser.newContext({ ignoreHTTPSErrors: true });
    const page = await ctx.newPage();
    await page.goto('/');

    // Register first user (gets admin role)
    await apiCall(page, '/api/v1/register', { username: ADMIN_USER, password: ADMIN_PASS });
    await apiCall(page, '/api/v1/login', { username: ADMIN_USER, password: ADMIN_PASS });

    // Create products as admin
    for (const p of [{ name: 'e2e-10积分礼包', price: 10, stock: 999 },
                     { name: 'e2e-50积分礼包', price: 50, stock: 500 }]) {
      await apiCall(page, '/api/v1/mall/products', p);
    }

    // Register regular user
    await apiCall(page, '/api/v1/register', { username: USER, password: PASS });
    await apiCall(page, '/api/v1/login', { username: USER, password: PASS });

    // Create a sheet for regular user
    await apiCall(page, '/api/v1/sheets', { name: 'e2e-test-sheet', headers_json: '[]', data_json: '[]' });

    // Create workspace
    await apiCall(page, '/api/v1/workspaces', { name: 'e2e-workspace' });

    await ctx.close();
  });

  // ==== Admin Tests ====

  test('admin can login and see admin controls', async ({ page }) => {
    await loginViaAPI(page, ADMIN_USER, ADMIN_PASS);
    await page.click('[data-tab="mall"]');
    await page.waitForTimeout(500);

    // Admin toggle visible
    await expect(page.locator('#mall-admin-toggle')).toBeAttached();
  });

  test('admin creates product via UI', async ({ page }) => {
    await loginViaAPI(page, ADMIN_USER, ADMIN_PASS);
    await page.click('[data-tab="mall"]');
    await page.waitForTimeout(500);

    // Open admin panel
    await page.locator('#mall-admin-toggle button').click();
    await page.waitForTimeout(300);

    await page.fill('#admin-prod-name', 'ui-created-product');
    await page.fill('#admin-prod-price', '30');
    await page.fill('#admin-prod-stock', '50');
    page.once('dialog', d => d.accept());
    await page.locator('#mall-admin-panel').locator('text=添加商品').click();
    await page.waitForTimeout(500);
  });

  test('admin creates seckill via UI', async ({ page }) => {
    await loginViaAPI(page, ADMIN_USER, ADMIN_PASS);
    await page.click('[data-tab="mall"]');
    await page.waitForTimeout(500);

    await page.locator('#mall-admin-toggle button').click();
    await page.waitForTimeout(300);

    const now = Math.floor(Date.now() / 1000);
    await page.fill('#admin-sk-prod-id', '1');
    await page.fill('#admin-sk-price', '5');
    await page.fill('#admin-sk-stock', '20');
    await page.fill('#admin-sk-start', String(now - 10));
    await page.fill('#admin-sk-end', String(now + 3600));
    page.once('dialog', d => d.accept());
    await page.locator('#mall-admin-panel').locator('text=创建秒杀').click();
    await page.waitForTimeout(500);
  });

  // ==== Regular User Tests ====

  test('sheets tab shows list', async ({ page }) => {
    await loginViaAPI(page, USER, PASS);
    await page.click('[data-tab="sheets"]');
    await page.waitForTimeout(500);
    await expect(page.locator('.sheet-card').first()).toBeAttached({ timeout: 5000 });
  });

  test('open a sheet', async ({ page }) => {
    await loginViaAPI(page, USER, PASS);
    await page.click('[data-tab="sheets"]');
    await page.waitForTimeout(500);
    await page.locator('.sheet-card').first().locator('text=打开').click();
    await page.waitForTimeout(500);
    await expect(page.locator('#sheet-grid')).toBeAttached();
  });

  test('points tab shows balance', async ({ page }) => {
    await loginViaAPI(page, USER, PASS);
    await page.click('[data-tab="points"]');
    await page.waitForTimeout(500);
    await expect(page.locator('#points-balance-display')).toBeAttached();
  });

  test('points tab shows rules', async ({ page }) => {
    await loginViaAPI(page, USER, PASS);
    await page.click('[data-tab="points"]');
    await page.waitForTimeout(500);
    await expect(page.locator('.rules-list')).toBeAttached();
  });

  test('workspace tab shows list', async ({ page }) => {
    await loginViaAPI(page, USER, PASS);
    await page.click('[data-tab="workspace"]');
    await page.waitForTimeout(500);
    await expect(page.locator('.ws-item').first()).toBeAttached({ timeout: 5000 });
  });

  test('mall tab shows products', async ({ page }) => {
    await loginViaAPI(page, USER, PASS);
    await page.click('[data-tab="mall"]');
    await page.waitForTimeout(1000);
    await expect(page.locator('#mall-products')).toBeAttached();
    await expect(page.locator('#mall-seckills')).toBeAttached();
    await expect(page.locator('#mall-orders')).toBeAttached();
  });

  test('files tab loads', async ({ page }) => {
    await loginViaAPI(page, USER, PASS);
    await page.click('[data-tab="files"]');
    await page.waitForTimeout(500);
    await expect(page.locator('#files-list')).toBeAttached();
  });

  test('share button visible on sheet card', async ({ page }) => {
    await loginViaAPI(page, USER, PASS);
    await page.click('[data-tab="sheets"]');
    await page.waitForTimeout(500);
    await expect(page.locator('.sheet-card').first().locator('text=分享')).toBeAttached({ timeout: 5000 });
  });

});

async function apiCall(page, path, body) {
  await page.evaluate(async ({ path, body }) => {
    await fetch(path, {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
  }, { path, body });
}

async function loginViaAPI(page, user, pass) {
  await page.goto('/');
  await apiCall(page, '/api/v1/login', { username: user, password: pass });
  await page.reload();
  await page.waitForTimeout(1000);
}
