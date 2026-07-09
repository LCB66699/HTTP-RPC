const { test, expect } = require('@playwright/test');

const ADMIN_USER = 'e2eadmin_' + Date.now();
const ADMIN_PASS = 'admin1234';

async function registerViaAPI(request, user, pass) {
  return request.post('/api/v1/register', {
    data: { username: user, password: pass },
    headers: { 'Content-Type': 'application/json' },
    failOnStatusCode: false,
  });
}

async function loginViaAPI(request, user, pass) {
  return request.post('/api/v1/login', {
    data: { username: user, password: pass },
    headers: { 'Content-Type': 'application/json' },
  });
}

test.describe.serial('HTTP-RPC E2E', () => {

  // ==== ADMIN SETUP ====
  test('admin registers and creates product', async ({ request, page }) => {
    // Register via API
    await registerViaAPI(request, ADMIN_USER, ADMIN_PASS);
    const loginResp = await loginViaAPI(request, ADMIN_USER, ADMIN_PASS);

    // Extract cookies from API response and inject into browser
    const cookies = loginResp.headers()['set-cookie'];
    if (cookies) {
      const parsed = cookies.split(';').map(c => {
        const [name, ...rest] = c.trim().split('=');
        return { name, value: rest.join('='), domain: 'localhost', path: '/' };
      });
      await page.context().addCookies(parsed.filter(c => c.name === 'rpc_at'));
    }

    await page.goto('/');
    await page.waitForTimeout(2000);

    // Navigate to mall and open admin panel
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

  async function registerAndLogin(request, page, user, pass) {
    await registerViaAPI(request, user, pass);
    const resp = await loginViaAPI(request, user, pass);
    const cookies = resp.headers()['set-cookie'];
    if (cookies) {
      const parsed = cookies.split(';').map(c => {
        const [name, ...rest] = c.trim().split('=');
        return { name, value: rest.join('='), domain: 'localhost', path: '/' };
      });
      await page.context().addCookies(parsed.filter(c => c.name === 'rpc_at'));
    }
    await page.goto('/');
    await page.waitForTimeout(2000);
  }

  test('sheets tab', async ({ request, page }) => {
    await registerAndLogin(request, page, 'e2euser_' + Date.now(), 'test1234');
    await page.click('[data-tab="sheets"]');
    await page.waitForTimeout(500);
    await expect(page.locator('#sheets-list')).toBeAttached();
  });

  test('create sheet', async ({ request, page }) => {
    await registerAndLogin(request, page, 'e2euser2_' + Date.now(), 'test1234');
    await page.click('[data-tab="sheets"]');
    await page.waitForTimeout(500);
    await page.click('#btn-sheet-create-blank');
    await page.waitForTimeout(1000);
    await expect(page.locator('.sheet-card').first()).toBeAttached({ timeout: 5000 });
  });

  test('points tab', async ({ request, page }) => {
    await registerAndLogin(request, page, 'e2euser3_' + Date.now(), 'test1234');
    await page.click('[data-tab="points"]');
    await page.waitForTimeout(500);
    await expect(page.locator('#points-balance-display')).toBeAttached();
  });

  test('workspace tab', async ({ request, page }) => {
    await registerAndLogin(request, page, 'e2euser4_' + Date.now(), 'test1234');
    await page.click('[data-tab="workspace"]');
    await page.waitForTimeout(500);
    await expect(page.locator('#workspace-list')).toBeAttached();
  });

  test('mall tab', async ({ request, page }) => {
    await registerAndLogin(request, page, 'e2euser5_' + Date.now(), 'test1234');
    await page.click('[data-tab="mall"]');
    await page.waitForTimeout(1000);
    await expect(page.locator('#mall-products')).toBeAttached();
  });

  test('files tab', async ({ request, page }) => {
    await registerAndLogin(request, page, 'e2euser6_' + Date.now(), 'test1234');
    await page.click('[data-tab="files"]');
    await page.waitForTimeout(500);
    await expect(page.locator('#files-list')).toBeAttached();
  });

});
