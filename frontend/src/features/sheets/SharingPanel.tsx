import { useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { Link2, Trash2, UserPlus } from 'lucide-react'
import { z } from 'zod'
import type { SharePermission } from '../../lib/api'
import { useSession } from '../../lib/session'
import { ErrorState, LoadingState } from '../../components/Page'

const usernameSchema = z.string().trim().min(3, 'Enter a username with at least 3 characters.').max(64, 'Username is too long.').regex(/^[A-Za-z0-9_.-]+$/, 'Use letters, numbers, dots, hyphens, or underscores only.')

function responseError<T extends { success: boolean; error?: string }>(response: T): T {
  if (!response.success) throw new Error(response.error || 'The sharing operation was not accepted.')
  return response
}

export function SharingPanel({ sheetId }: { sheetId: string | number }) {
  const { api } = useSession()
  const client = useQueryClient()
  const [username, setUsername] = useState('')
  const [permission, setPermission] = useState<SharePermission>('view')
  const [validationError, setValidationError] = useState('')
  const shares = useQuery({ queryKey: ['sheet-shares', sheetId], queryFn: () => api.listSheetShares(sheetId) })
  const refresh = () => client.invalidateQueries({ queryKey: ['sheet-shares', sheetId] })
  const grant = useMutation({
    mutationFn: ({ grantee, level }: { grantee: string; level: SharePermission }) => api.shareSheet(sheetId, grantee, level).then(responseError),
    onSuccess: () => { setUsername(''); return refresh() }
  })
  const revoke = useMutation({ mutationFn: (grantee: string) => api.revokeSheetShare(sheetId, grantee).then(responseError), onSuccess: refresh })
  const link = useMutation({ mutationFn: () => api.createSheetShareLink(sheetId).then(responseError) })
  const shareLink = link.data?.token ? `${window.location.origin}/api/v1/s/${link.data.token}` : ''

  const submit = () => {
    const parsed = usernameSchema.safeParse(username)
    if (!parsed.success) { setValidationError(parsed.error.issues[0]?.message || 'Enter a valid username.'); return }
    setValidationError('')
    grant.mutate({ grantee: parsed.data, level: permission })
  }

  return <section aria-label="Spreadsheet sharing" className="collaboration-panel">
    <div><h2>Sharing</h2><p>Grant access by account name or create a read-only link.</p></div>
    <div className="toolbar compact-toolbar">
      <input aria-label="Share with username" onChange={event => setUsername(event.target.value)} placeholder="Username" value={username} />
      <select aria-label="Share permission" onChange={event => setPermission(event.target.value as SharePermission)} value={permission}><option value="view">Can view</option><option value="edit">Can edit</option></select>
      <button disabled={grant.isPending} onClick={submit} type="button"><UserPlus aria-hidden="true" size={17} /> Add member</button>
    </div>
    {validationError && <p className="form-error">{validationError}</p>}
    {(grant.isError || revoke.isError || link.isError) && <ErrorState message={(grant.error || revoke.error || link.error)?.message || 'The sharing operation failed.'} />}
    {shares.isPending && <LoadingState label="Loading shared members..." />}
    {shares.isError && <ErrorState message="Shared members could not be loaded." />}
    {shares.data?.entries?.length ? <div className="data-list">{shares.data.entries.map(entry => <div className="data-row" key={entry.username}><span>{entry.username}</span><small>{entry.permission}</small><button aria-label={`Remove ${entry.username}`} className="icon-button danger" disabled={revoke.isPending} onClick={() => revoke.mutate(entry.username)} title="Remove member" type="button"><Trash2 aria-hidden="true" size={17} /></button></div>)}</div> : !shares.isPending && <p className="muted">No direct collaborators yet.</p>}
    <div className="link-row"><button className="secondary" disabled={link.isPending} onClick={() => link.mutate()} type="button"><Link2 aria-hidden="true" size={17} /> {link.isPending ? 'Creating link...' : 'Create view link'}</button>{shareLink && <input aria-label="Read-only share link" readOnly value={shareLink} />}</div>
  </section>
}
