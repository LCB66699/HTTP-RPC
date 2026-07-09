const { test, expect } = require('@playwright/test');

const ADMIN_USER = 'e2eadmin_' + Date.now();
const ADMIN_PASS = 'admin1234';
const USER = 'e2euser_' + Date.now();
const PASS = 'test1234';

async function loginViaUI(page, user, pass) {
  await page.goto('/');
  // Fill login form and submit like a real user
  await page.fill('input#login-username', user);
  await page.fill('input#login-password', pass);
  await page.click('#login-form button[type="submit"]');
  // Wait for main app to appear
  await page.waitForSelector('#main-header', { state: 'visible', timeout: 15000 });
}

test.describe('HTTP-RPC E2E', () => {

  // ---- Admin setup ----
  test('admin registers and creates products', async ({ page }) => {
    await page.goto('/');

    // Register admin (first user = admin role)
    await page.click('#btn-show-register');
    await page.fill('#reg-username', ADMIN_USER);
    await page.fill('#reg-password', ADMIN_PASS);
    await page.click('#register-form button[type="submit"]');
    await page.waitForSelector('#main-header', { state: 'visible', timeout: 15000 });

    // Open admin panel
    await page.click('[data-tab="mall"]');
    await page.waitForTimeout(500);
    await page.locator('#mall-admin-toggle button').click();
    await page.waitForTimeout(300);

    // Create product
    await page.fill('#admin-prod-name', 'e2e-test-product');
    await page.fill('#admin-prod-price', '20');
    await page.fill('#admin-prod-stock', '100');
    page.once('dialog', d => d.accept());
    await page.locator('#mall-admin-panel').locator('text=添加商品').click();
    await page.waitForTimeout(500);

    // Create seckill
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

  // ---- Regular user tests ----
  test.beforeEach(async ({ page }) => {
    // Register + login regular user
    await page.goto('/');
    await page.click('#btn-show-register');
    await page.fill('#reg-username', USER);
    await page.fill('#reg-password', PASS);
    await page.click('#register-form button[type="submit"]');
    await page.waitForSelector('#main-header', { state: 'visible', timeout: 15000 });
  });

  test('sheets tab shows list', async ({ page }) => {
    await page.click('[data-tab="sheets"]');
    await page.waitForTimeout(500);
    await expect(page.locator('#sheets-list')).toBeAttached();
  });

  test('create blank sheet via button', async ({ page }) => {
    await page.click('[data-tab="sheets"]');
    await page.waitForTimeout(500);
    await page.click('#btn-sheet-create-blank');
    await page.waitForTimeout(1000);
    await expect(page.locator('.sheet-card').first()).toBeAttached({ timeout: 5000 });
  });

  test('points tab shows balance', async ({ page }) => {
    await page.click('[data-tab="points"]');
    await page.waitForTimeout(500);
    await expect(page.locator('#points-balance-display')).toBeAttached();
  });

  test('workspace tab shows list', async ({ page }) => {
    await page.click('[data-tab="workspace"]');
    await page.waitForTimeout(500);
    await expect(page.locator('#workspace-list')).toBeAttached();
  });

  test('mall tab shows products and seckills', async ({ page }) => {
    await page.click('[data-tab="mall"]');
    await page.waitForTimeout(1000);
    await expect(page.locator('#mall-products')).toBeAttached();
    await expect(page.locator('#mall-seckills')).toBeAttached();
  });

  test('files tab loads', async ({ page }) => {
    await page.click('[data-tab="files"]');
    await page.waitForTimeout(500);
    await expect(page.locator('#files-list')).toBeAttached();
  });

  test('share button visible on sheet card', async ({ page }) => {
    await page.click('[data-tab="sheets"]');
    await page.waitForTimeout(500);
    // Create a sheet first if none exist
    await page.click('#btn-sheet-create-blank');
    await page.waitForTimeout(1000);
    await expect(page.locator('.sheet-card').first().locator('text=分享')).toBeAttached({ timeout: 5000 });
  });

});
