import { createContext, useContext } from 'react'
import type { ApiClient, User } from './api'

export interface Session {
  api: ApiClient
  user: User
  signOut: () => Promise<void>
}

export const SessionContext = createContext<Session | undefined>(undefined)

export function useSession() {
  const session = useContext(SessionContext)
  if (!session) throw new Error('SessionContext is required for protected pages')
  return session
}
