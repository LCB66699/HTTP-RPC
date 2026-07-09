const { test, expect } = require('@playwright/test');

const TEST_USER = 'e2e_' + Date.now();
const TEST_PASS = 'test1234';

test.describe('HTTP-RPC E2E', () => {

  test.beforeAll(async ({ browser }) => {
    const ctx = await browser.newContext({ ignoreHTTPSErrors: true });
    const page = await ctx.newPage();
    await page.goto('/');
    await page.evaluate(async ({ user, pass }) => {
      await fetch('/api/v1/register', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ username: user, password: pass }),
      });
      await fetch('/api/v1/login', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ username: user, password: pass }),
      });
    }, { user: TEST_USER, pass: TEST_PASS });
    await ctx.close();
  });

  // ---- Sheets ----

  test('create a sheet', async ({ page }) => {
    await loginViaAPI(page);
    await page.click('[data-tab="sheets"]');
    await page.waitForTimeout(500);

    await page.click('#btn-sheet-create-blank');
    await page.waitForTimeout(1000);
    await expect(page.locator('.sheet-card').first()).toBeAttached();
  });

  test('open and edit a sheet', async ({ page }) => {
    await loginViaAPI(page);
    await page.click('[data-tab="sheets"]');
    await page.waitForTimeout(500);

    await page.locator('.sheet-card').first().locator('text=打开').click();
    await page.waitForTimeout(500);
    await expect(page.locator('#sheet-grid')).toBeAttached();
  });

  test('delete a sheet', async ({ page }) => {
    await loginViaAPI(page);
    await page.click('[data-tab="sheets"]');
    await page.waitForTimeout(500);

    const countBefore = await page.locator('.sheet-card').count();
    await page.locator('.sheet-card').first().locator('text=删除').click();
    await page.waitForTimeout(500);
    await expect(page.locator('.sheet-card')).toHaveCount(countBefore - 1);
  });

  // ---- Points ----

  test('points tab has balance and rules', async ({ page }) => {
    await loginViaAPI(page);
    await page.click('[data-tab="points"]');
    await page.waitForTimeout(500);
    await expect(page.locator('#points-balance-display')).toBeAttached();
    await expect(page.locator('.rules-list')).toBeAttached();
  });

  test('points transactions load', async ({ page }) => {
    await loginViaAPI(page);
    await page.click('[data-tab="points"]');
    await page.waitForTimeout(500);
    await expect(page.locator('#points-transactions')).toBeAttached();
  });

  // ---- Workspace ----

  test('create workspace', async ({ page }) => {
    await loginViaAPI(page);
    await page.click('[data-tab="workspace"]');
    await page.waitForTimeout(500);

    // Handle prompt
    page.on('dialog', async dialog => { await dialog.accept('e2e-workspace'); });
    await page.click('text=+ 新建');
    await page.waitForTimeout(1000);
    await expect(page.locator('#workspace-list')).toBeAttached();
  });

  // ---- Mall Admin ----

  test('admin creates product', async ({ page }) => {
    await loginViaAPI(page);
    await page.click('[data-tab="mall"]');
    await page.waitForTimeout(500);

    // Open admin panel
    const toggle = page.locator('#mall-admin-toggle');
    if (await toggle.isVisible().catch(() => false)) {
      await toggle.locator('button').click();
      await page.waitForTimeout(300);
    }

    await page.fill('#admin-prod-name', 'e2e-test-product');
    await page.fill('#admin-prod-price', '20');
    await page.fill('#admin-prod-stock', '100');
    page.once('dialog', d => d.accept());
    await page.click('text=添加商品');
    await page.waitForTimeout(500);
  });

  test('admin creates seckill', async ({ page }) => {
    await loginViaAPI(page);
    await page.click('[data-tab="mall"]');
    await page.waitForTimeout(500);

    const toggle = page.locator('#mall-admin-toggle');
    if (await toggle.isVisible().catch(() => false)) {
      await toggle.locator('button').click();
      await page.waitForTimeout(300);
    }

    const now = Math.floor(Date.now() / 1000);
    await page.fill('#admin-sk-prod-id', '1');
    await page.fill('#admin-sk-price', '10');
    await page.fill('#admin-sk-stock', '50');
    await page.fill('#admin-sk-start', String(now - 10));
    await page.fill('#admin-sk-end', String(now + 3600));
    page.once('dialog', d => d.accept());
    await page.click('text=创建秒杀');
    await page.waitForTimeout(500);
  });

  // ---- Mall Orders ----

  test('normal order via product exchange', async ({ page }) => {
    await loginViaAPI(page);
    await page.click('[data-tab="mall"]');
    await page.waitForTimeout(1000);

    const btn = page.locator('.product-card').first().locator('text=兑换');
    if (await btn.isVisible().catch(() => false)) {
      page.once('dialog', d => d.accept());
      await btn.click();
      await page.waitForTimeout(1000);
    }
  });

  test('mall tab loads', async ({ page }) => {
    await loginViaAPI(page);
    await page.click('[data-tab="mall"]');
    await page.waitForTimeout(1000);
    await expect(page.locator('#mall-products')).toBeAttached();
    await expect(page.locator('#mall-seckills')).toBeAttached();
    await expect(page.locator('#mall-orders')).toBeAttached();
  });

  // ---- Files ----

  test('files tab loads', async ({ page }) => {
    await loginViaAPI(page);
    await page.click('[data-tab="files"]');
    await page.waitForTimeout(500);
    await expect(page.locator('#files-list')).toBeAttached();
  });

});

async function loginViaAPI(page) {
  await page.goto('/');
  await page.evaluate(async ({ user, pass }) => {
    await fetch('/api/v1/login', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      credentials: 'same-origin',
      body: JSON.stringify({ username: user, password: pass }),
    });
  }, { user: TEST_USER, pass: TEST_PASS });
  await page.reload();
  await page.waitForTimeout(1000);
}
