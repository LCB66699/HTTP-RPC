export interface User {
  user_id: number
  username: string
  role?: string
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

    if (!response.ok) {
      throw new ApiError(response.status, await errorMessage(response))
    }

    if (response.status === 204) return undefined as T
    return response.json() as Promise<T>
  }

  return {
    getMe: () => request<User>('/me'),
    listServices: async () => {
      const response = await request<{ services?: RegisteredService[] }>('/services')
      return response.services ?? []
    },
    login: (input: LoginInput) => request<LoginResponse>('/login', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(input)
    }),
    logout: () => request<void>('/logout', { method: 'POST' })
  }
}
