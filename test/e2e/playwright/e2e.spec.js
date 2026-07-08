const { test, expect } = require('@playwright/test');

const TEST_USER = 'e2e_' + Date.now();
const TEST_PASS = 'test1234';

test.describe('HTTP-RPC E2E', () => {

  test('register and login', async ({ page }) => {
    await page.goto('/');

    // Click "立即注册" button to show register form
    await page.click('#btn-show-register');
    await page.fill('#reg-username', TEST_USER);
    await page.fill('#reg-password', TEST_PASS);
    await page.locator('#register-form button[type="submit"]').click();

    await page.waitForSelector('#main-header:not(.hidden)', { timeout: 10000 });
    await expect(page.locator('#main-header')).toBeVisible();
  });

  test('create and open a sheet', async ({ page }) => {
    await login(page);

    await page.click('[data-tab="sheets"]');
    await page.waitForTimeout(500);

    // Click the "新建空白表格" button which calls createBlankSheet()
    await page.click('#btn-sheet-create-blank');
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

  if (await page.locator('#main-header:not(.hidden)').isVisible({ timeout: 1000 }).catch(() => false)) {
    return;
  }

  await page.fill('#login-username', TEST_USER);
  await page.fill('#login-password', TEST_PASS);
  await page.locator('#login-form button[type="submit"]').click();

  await page.waitForSelector('#main-header:not(.hidden)', { timeout: 8000 });
}
