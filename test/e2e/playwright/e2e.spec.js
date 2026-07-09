const { test, expect } = require('@playwright/test');

const TEST_USER = 'e2e_' + Date.now();
const TEST_PASS = 'test1234';

test.describe('HTTP-RPC E2E', () => {

  test.beforeAll(async ({ browser }) => {
    // Register and login via API — bypass UI form issues in headless mode
    const ctx = await browser.newContext({ ignoreHTTPSErrors: true });
    const page = await ctx.newPage();
    await page.goto('/');

    // Register
    await page.evaluate(async (user, pass) => {
      const resp = await fetch('/api/v1/register', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ username: user, password: pass }),
      });
      return resp.json();
    }, TEST_USER, TEST_PASS);

    // Login
    await page.evaluate(async (user, pass) => {
      const resp = await fetch('/api/v1/login', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ username: user, password: pass }),
      });
      return resp.json();
    }, TEST_USER, TEST_PASS);

    await page.reload();
    await page.waitForTimeout(1000);
    await ctx.close();
  });

  test('home page loads after login', async ({ page }) => {
    await loginViaAPI(page);
    await expect(page.locator('#main-header')).toBeAttached();
  });

  test('points tab loads', async ({ page }) => {
    await loginViaAPI(page);
    await page.click('[data-tab="points"]');
    await page.waitForTimeout(500);
    await expect(page.locator('#points-balance-display')).toBeAttached();
  });

  test('mall tab loads products', async ({ page }) => {
    await loginViaAPI(page);
    await page.click('[data-tab="mall"]');
    await page.waitForTimeout(1000);
    await expect(page.locator('#mall-products')).toBeAttached();
  });

  test('workspace tab loads', async ({ page }) => {
    await loginViaAPI(page);
    await page.click('[data-tab="workspace"]');
    await page.waitForTimeout(500);
    await expect(page.locator('#workspace-list')).toBeAttached();
  });

});

async function loginViaAPI(page) {
  await page.goto('/');

  // Register + login via API (idempotent — register returns success or already-exists)
  await page.evaluate(async (user, pass) => {
    await fetch('/api/v1/register', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username: user, password: pass }),
    });
    await fetch('/api/v1/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      credentials: 'same-origin',
      body: JSON.stringify({ username: user, password: pass }),
    });
  }, TEST_USER, TEST_PASS);

  // Reload so the cookie takes effect and showMainApp() runs
  await page.reload();
  await page.waitForTimeout(1000);
}
