const { test, expect } = require('@playwright/test');

const ADMIN_USER = 'e2eadmin_' + Date.now();
const ADMIN_PASS = 'admin1234';
const USER = 'e2euser_' + Date.now();
const PASS = 'test1234';

async function login(page, user, pass) {
  await page.fill('input#login-username', user);
  await page.fill('input#login-password', pass);
  await page.click('#login-form button[type="submit"]');
  await page.waitForSelector('#main-header', { state: 'visible', timeout: 15000 });
}

async function register(page, user, pass) {
  await page.click('#btn-show-register');
  await page.fill('#reg-username', user);
  await page.fill('#reg-password', pass);
  await page.click('#register-form button[type="submit"]');
  await page.waitForSelector('#main-header', { state: 'visible', timeout: 15000 });
}

test.describe.serial('HTTP-RPC E2E', () => {

  // admin setup
  test('admin registers', async ({ page }) => {
    await page.goto('/');
    await register(page, ADMIN_USER, ADMIN_PASS);
  });

  test('admin creates product and seckill', async ({ page }) => {
    await page.goto('/');
    await login(page, ADMIN_USER, ADMIN_PASS);
    await page.click('[data-tab="mall"]');
    await page.waitForTimeout(500);
    await page.locator('#mall-admin-toggle button').click();
    await page.waitForTimeout(300);

    await page.fill('#admin-prod-name', 'e2e-test-product');
    await page.fill('#admin-prod-price', '20');
    await page.fill('#admin-prod-stock', '100');
    page.once('dialog', d => d.accept());
    await page.locator('#mall-admin-panel').locator('text=添加商品').click();
    await page.waitForTimeout(500);

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

  // regular user
  test('regular user registers', async ({ page }) => {
    await page.goto('/');
    await register(page, USER, PASS);
  });

  test('create sheet and open', async ({ page }) => {
    await page.goto('/');
    await login(page, USER, PASS);
    await page.click('[data-tab="sheets"]');
    await page.waitForTimeout(500);
    await page.click('#btn-sheet-create-blank');
    await page.waitForTimeout(1000);
    await expect(page.locator('.sheet-card').first()).toBeAttached({ timeout: 5000 });
    await page.locator('.sheet-card').first().locator('text=打开').click();
    await page.waitForTimeout(500);
    await expect(page.locator('#sheet-grid')).toBeAttached();
  });

  test('points tab', async ({ page }) => {
    await page.goto('/');
    await login(page, USER, PASS);
    await page.click('[data-tab="points"]');
    await page.waitForTimeout(500);
    await expect(page.locator('#points-balance-display')).toBeAttached();
  });

  test('workspace tab', async ({ page }) => {
    await page.goto('/');
    await login(page, USER, PASS);
    await page.click('[data-tab="workspace"]');
    await page.waitForTimeout(500);
    await expect(page.locator('#workspace-list')).toBeAttached();
  });

  test('mall tab', async ({ page }) => {
    await page.goto('/');
    await login(page, USER, PASS);
    await page.click('[data-tab="mall"]');
    await page.waitForTimeout(1000);
    await expect(page.locator('#mall-products')).toBeAttached();
  });

  test('files tab', async ({ page }) => {
    await page.goto('/');
    await login(page, USER, PASS);
    await page.click('[data-tab="files"]');
    await page.waitForTimeout(500);
    await expect(page.locator('#files-list')).toBeAttached();
  });

});
