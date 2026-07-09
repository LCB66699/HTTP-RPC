const { test, expect } = require('@playwright/test');

const ADMIN_USER = 'e2eadmin_' + Date.now();
const ADMIN_PASS = 'admin1234';
const BASE = 'https://localhost';

test.describe.serial('HTTP-RPC E2E', () => {

  let loggedInContext;

  // ====== ADMIN SETUP (via API, then save browser state) ======
  test('setup: register admin and login', async ({ browser }) => {
    const ctx = await browser.newContext({ ignoreHTTPSErrors: true });
    const page = await ctx.newPage();

    // Register + login via UI
    await page.goto(BASE + '/');
    await page.click('#btn-show-register');
    await page.fill('#reg-username', ADMIN_USER);
    await page.fill('#reg-password', ADMIN_PASS);
    await page.click('#register-form button[type="submit"]');
    await page.waitForSelector('#main-header', { state: 'visible', timeout: 15000 });

    // Save browser state (cookies, localStorage)
    loggedInContext = await ctx.storageState();
    await ctx.close();
  });

  // ====== ALL OTHER TESTS: reuse saved browser state ======
  test('admin creates product and seckill', async ({ browser }) => {
    const ctx = await browser.newContext({ ignoreHTTPSErrors: true, storageState: loggedInContext });
    const page = await ctx.newPage();
    await page.goto(BASE + '/');
    await page.waitForSelector('#main-header', { state: 'visible', timeout: 10000 });

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
    await ctx.close();
  });

  test('sheets tab', async ({ browser }) => {
    const ctx = await browser.newContext({ ignoreHTTPSErrors: true, storageState: loggedInContext });
    const page = await ctx.newPage();
    await page.goto(BASE + '/');
    await page.waitForSelector('#main-header', { state: 'visible', timeout: 10000 });
    await page.click('[data-tab="sheets"]');
    await page.waitForTimeout(500);
    await expect(page.locator('#sheets-list')).toBeAttached();
    await ctx.close();
  });

  test('create sheet', async ({ browser }) => {
    const ctx = await browser.newContext({ ignoreHTTPSErrors: true, storageState: loggedInContext });
    const page = await ctx.newPage();
    await page.goto(BASE + '/');
    await page.waitForSelector('#main-header', { state: 'visible', timeout: 10000 });
    await page.click('[data-tab="sheets"]');
    await page.waitForTimeout(500);
    await page.click('#btn-sheet-create-blank');
    await page.waitForTimeout(1000);
    await expect(page.locator('.sheet-card').first()).toBeAttached({ timeout: 5000 });
    await ctx.close();
  });

  test('points tab', async ({ browser }) => {
    const ctx = await browser.newContext({ ignoreHTTPSErrors: true, storageState: loggedInContext });
    const page = await ctx.newPage();
    await page.goto(BASE + '/');
    await page.waitForSelector('#main-header', { state: 'visible', timeout: 10000 });
    await page.click('[data-tab="points"]');
    await page.waitForTimeout(500);
    await expect(page.locator('#points-balance-display')).toBeAttached();
    await ctx.close();
  });

  test('workspace tab', async ({ browser }) => {
    const ctx = await browser.newContext({ ignoreHTTPSErrors: true, storageState: loggedInContext });
    const page = await ctx.newPage();
    await page.goto(BASE + '/');
    await page.waitForSelector('#main-header', { state: 'visible', timeout: 10000 });
    await page.click('[data-tab="workspace"]');
    await page.waitForTimeout(500);
    await expect(page.locator('#workspace-list')).toBeAttached();
    await ctx.close();
  });

  test('mall tab', async ({ browser }) => {
    const ctx = await browser.newContext({ ignoreHTTPSErrors: true, storageState: loggedInContext });
    const page = await ctx.newPage();
    await page.goto(BASE + '/');
    await page.waitForSelector('#main-header', { state: 'visible', timeout: 10000 });
    await page.click('[data-tab="mall"]');
    await page.waitForTimeout(1000);
    await expect(page.locator('#mall-products')).toBeAttached();
    await ctx.close();
  });

  test('files tab', async ({ browser }) => {
    const ctx = await browser.newContext({ ignoreHTTPSErrors: true, storageState: loggedInContext });
    const page = await ctx.newPage();
    await page.goto(BASE + '/');
    await page.waitForSelector('#main-header', { state: 'visible', timeout: 10000 });
    await page.click('[data-tab="files"]');
    await page.waitForTimeout(500);
    await expect(page.locator('#files-list')).toBeAttached();
    await ctx.close();
  });

});
