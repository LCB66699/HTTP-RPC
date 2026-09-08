export interface User {
  user_id: number
  username: string
  role?: string
}

export interface LoginInput {
  username: string
  password: string
}

export interface LoginResponse {
  success: boolean
  username?: string
  user_id?: number
  role?: string
  error?: string
}

export interface ServiceMethod {
  name: string
  description?: string
}

export interface RegisteredService {
  name: string
  description?: string
  methods?: ServiceMethod[]
}

export interface HealthState {
  gateway: string
  [service: string]: string | { breaker?: string; channel?: string }
}

export interface Sheet {
  id: number
  name: string
  description?: string
  headers_json: string | string[]
  data_json: string | string[][]
  row_count?: number
  col_count?: number
  updated_at?: string
}

export interface SheetListResponse {
  success?: boolean
  sheets?: Sheet[]
  total?: number
}

export interface SheetResponse {
  success: boolean
  spreadsheet?: Sheet
  id?: number
  cache_source?: string
  error?: string
}

export interface FileEntry {
  id: number
  original_name: string
  size?: number
  mime_type?: string
  is_folder?: boolean
  parent_folder_id?: number
  created_at?: string
}

export interface FileListResponse {
  success?: boolean
  files?: FileEntry[]
  total?: number
}

export interface WorkspaceMember {
  user_id: number
  username?: string
  role?: string
}

export interface Workspace {
  id: number
  name: string
  owner_id: number
  members?: WorkspaceMember[]
}

export interface Product {
  id: number
  name: string
  description?: string
  price: number
  stock?: number
}

export interface Seckill {
  id: number
  product_id?: number
  product_name?: string
  seckill_price: number
  seckill_stock?: number
  start_at?: number
  end_at?: number
}

export interface Order {
  id: number
  product_name?: string
  seckill_id?: number
  amount?: number
  status?: string
  created_at?: string
}

export interface Transaction {
  id: number
  type: string
  amount: number
  reason?: string
  created_at?: string
}

export interface SearchResult {
  id: number
  type?: string
  name?: string
  title?: string
  snippet?: string
  updated_at?: string
  created_at?: string
}

type Fetcher = (input: RequestInfo | URL, init?: RequestInit) => Promise<Response>

export class ApiError extends Error {
  constructor(readonly status: number, message: string) {
    super(message)
    this.name = 'ApiError'
  }
}

function isJson(response: Response) {
  return response.headers.get('content-type')?.includes('application/json') ?? false
}

async function errorMessage(response: Response) {
  if (isJson(response)) {
    const body = await response.json().catch(() => null) as { error?: string } | null
    if (body?.error) return body.error
  }
  return `Request failed with status ${response.status}`
}

export function newIdempotencyKey() {
  return crypto.randomUUID()
}

export function createApiClient(fetcher: Fetcher) {
  let refreshInFlight: Promise<boolean> | undefined

  const refresh = async () => {
    if (!refreshInFlight) {
      refreshInFlight = fetcher('/api/v1/refresh', {
        method: 'POST',
        credentials: 'same-origin'
      })
        .then(response => response.ok)
        .catch(() => false)
        .finally(() => {
          refreshInFlight = undefined
        })
    }
    return refreshInFlight
  }

  const request = async <T>(path: string, init: RequestInit = {}, retry = true): Promise<T> => {
    const response = await fetcher(`/api/v1${path}`, {
      ...init,
      credentials: 'same-origin'
    })

    if (response.status === 401 && retry && path !== '/refresh' && await refresh()) {
      return request<T>(path, init, false)
    }

    if (!response.ok) throw new ApiError(response.status, await errorMessage(response))
    if (response.status === 204) return undefined as T
    return response.json() as Promise<T>
  }

  const json = <T>(path: string, method: 'POST' | 'PUT' | 'DELETE', body?: unknown, idempotent = false) =>
    request<T>(path, {
      method,
      headers: {
        'content-type': 'application/json',
        ...(idempotent ? { 'Idempotency-Key': newIdempotencyKey() } : {})
      },
      ...(body === undefined ? {} : { body: JSON.stringify(body) })
    })

  return {
    getMe: () => request<User>('/me'),
    getHealth: () => request<HealthState>('/health'),
    listServices: async (): Promise<RegisteredService[]> => {
      const response = await request<{ services?: Record<string, string[]> | RegisteredService[] }>('/services')
      if (Array.isArray(response.services)) return response.services
      return Object.entries(response.services ?? {}).map(([name, methods]) => ({
        name,
        methods: methods.map(name => ({ name }))
      }))
    },
    login: (input: LoginInput) => json<LoginResponse>('/login', 'POST', input),
    register: (input: LoginInput) => json<LoginResponse>('/register', 'POST', input),
    logout: () => json<void>('/logout', 'POST'),
    changePassword: (old_password: string, new_password: string) =>
      json<{ success: boolean; error?: string }>('/me/password', 'PUT', { old_password, new_password }),
    listSheets: () => request<SheetListResponse>('/sheets'),
    getSheet: (id: number) => request<SheetResponse>(`/sheets/${id}`),
    createSheet: (sheet: Pick<Sheet, 'name' | 'description' | 'headers_json' | 'data_json'>) =>
      json<SheetResponse>('/sheets', 'POST', sheet, true),
    updateSheet: (id: number, sheet: Pick<Sheet, 'name' | 'description' | 'headers_json' | 'data_json'>) =>
      json<SheetResponse>(`/sheets/${id}`, 'PUT', sheet),
    deleteSheet: (id: number) => json<{ success: boolean; error?: string }>(`/sheets/${id}`, 'DELETE'),
    listFiles: () => request<FileListResponse>('/files'),
    uploadFile: (file: File) => {
      const form = new FormData()
      form.append('file', file)
      return request<{ success: boolean; error?: string }>('/files/upload', {
        method: 'POST',
        headers: { 'Idempotency-Key': newIdempotencyKey() },
        body: form
      })
    },
    deleteFile: (id: number) => json<{ success: boolean; error?: string }>(`/files/${id}`, 'DELETE'),
    createFolder: (name: string, parent_folder_id = 0) =>
      json<{ success: boolean; error?: string }>('/files/folder', 'POST', { name, parent_folder_id }),
    moveFile: (id: number, target_folder_id: number) =>
      json<{ success: boolean; error?: string }>(`/files/${id}/move`, 'PUT', { target_folder_id }),
    listWorkspaces: () => request<{ success?: boolean; workspaces?: Workspace[] }>('/workspaces'),
    getWorkspace: (id: number) => request<{ success?: boolean; workspace?: Workspace }>(`/workspaces/${id}`),
    createWorkspace: (name: string) => json<{ success: boolean; error?: string }>('/workspaces', 'POST', { name }),
    updateWorkspace: (id: number, name: string) => json<{ success: boolean; error?: string }>(`/workspaces/${id}`, 'PUT', { name }),
    deleteWorkspace: (id: number) => json<{ success: boolean; error?: string }>(`/workspaces/${id}`, 'DELETE'),
    addWorkspaceMember: (id: number, username: string, role: string) =>
      json<{ success: boolean; error?: string }>(`/workspaces/${id}/members`, 'POST', { username, role }),
    removeWorkspaceMember: (id: number, userId: number) =>
      json<{ success: boolean; error?: string }>(`/workspaces/${id}/members/${userId}`, 'DELETE'),
    getBalance: () => request<{ success?: boolean; balance?: number }>('/points/balance'),
    getTransactions: () => request<{ success?: boolean; transactions?: Transaction[] }>('/points/transactions'),
    getLeaderboard: () => request<{ success?: boolean; entries?: Array<{ user_id: number; total_earned: number }> }>('/points/leaderboard'),
    listProducts: () => request<{ success?: boolean; products?: Product[] }>('/mall/products'),
    listSeckills: () => request<{ success?: boolean; seckills?: Seckill[] }>('/mall/seckills'),
    listOrders: () => request<{ success?: boolean; orders?: Order[] }>('/mall/orders'),
    orderProduct: (product_id: number) => json<{ success: boolean; error?: string }>('/mall/order', 'POST', { product_id }, true),
    orderSeckill: (seckill_id: number) => json<{ success: boolean; error?: string }>('/mall/seckill/order', 'POST', { seckill_id }, true),
    search: (q: string, sort = '') => json<{ success?: boolean; results?: SearchResult[]; total?: number }>('/search', 'POST', { q, sort }),
    getHistory: () => request<{ count?: number; entries?: string[] }>('/history')
  }
}

export type ApiClient = ReturnType<typeof createApiClient>
