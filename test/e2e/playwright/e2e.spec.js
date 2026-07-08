const { test, expect } = require('@playwright/test');

const TEST_USER = 'e2e_' + Date.now();
const TEST_PASS = 'test1234';

test.describe('HTTP-RPC E2E', () => {

  test('register and login', async ({ page }) => {
    await page.goto('/');

    await page.click('text=没有账号？立即注册');
    await page.fill('#reg-username', TEST_USER);
    await page.fill('#reg-password', TEST_PASS);
    await page.click('#register-submit');

    await page.waitForSelector('#points-balance-display', { timeout: 8000 });
    await expect(page.locator('#points-balance-display')).toBeVisible();
  });

  test('create and open a sheet', async ({ page }) => {
    await login(page);

    await page.click('[data-tab="sheets"]');
    await page.waitForTimeout(500);

    const name = 'e2e-' + Date.now();
    await page.fill('#sheet-name-input', name);
    await page.click('#sheet-create-btn');
    await page.waitForTimeout(1000);

    await expect(page.locator('.sheet-card').first()).toBeVisible();
    await page.locator('.sheet-card').first().locator('text=打开').click();
    await page.waitForTimeout(500);
  });

  test('points tab loads', async ({ page }) => {
    await login(page);

    await page.click('[data-tab="points"]');
    await page.waitForTimeout(500);

    await expect(page.locator('#points-balance-display')).toBeVisible();
    await expect(page.locator('.rules-list')).toBeVisible();
  });

  test('mall tab loads products', async ({ page }) => {
    await login(page);

    await page.click('[data-tab="mall"]');
    await page.waitForTimeout(1000);

    await expect(page.locator('#mall-products')).toBeVisible();
  });

  test('workspace tab loads', async ({ page }) => {
    await login(page);

    await page.click('[data-tab="workspace"]');
    await page.waitForTimeout(500);

    await expect(page.locator('#workspace-list')).toBeVisible();
  });

});

async function login(page) {
  await page.goto('/');

  if (await page.locator('#points-balance-display').isVisible({ timeout: 1000 }).catch(() => false)) {
    return;
  }

  await page.fill('#login-username', TEST_USER);
  await page.fill('#login-password', TEST_PASS);
  await page.click('#login-submit');

  await page.waitForSelector('#points-balance-display', { timeout: 8000 });
}
