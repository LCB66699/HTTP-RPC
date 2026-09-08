import { useEffect, useMemo, useRef, useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { Link, useNavigate, useParams } from '@tanstack/react-router'
import { useVirtualizer } from '@tanstack/react-virtual'
import { Download, FilePlus2, FolderPlus, Plus, Save, Trash2, Upload } from 'lucide-react'
import type { Sheet } from '../lib/api'
import { useSession } from '../lib/session'
import { EmptyState, ErrorState, LoadingState, PageHeader } from '../components/Page'

const gridKey = ['sheets'] as const
const fileKey = ['files'] as const
const workspaceKey = ['workspaces'] as const

function parseStringArray(value: string | string[]) {
  if (Array.isArray(value)) return value.map(String)
  try {
    const parsed: unknown = JSON.parse(value)
    return Array.isArray(parsed) ? parsed.map(String) : []
  } catch {
    return []
  }
}

function parseGrid(value: string | string[][]) {
  if (Array.isArray(value)) return value.map(row => row.map(String))
  try {
    const parsed: unknown = JSON.parse(value)
    return Array.isArray(parsed) ? parsed.map(row => Array.isArray(row) ? row.map(String) : []) : []
  } catch {
    return []
  }
}

function formatBytes(size = 0) {
  if (size < 1024) return `${size} B`
  if (size < 1024 ** 2) return `${(size / 1024).toFixed(1)} KB`
  return `${(size / 1024 ** 2).toFixed(1)} MB`
}

function parseCsv(text: string) {
  const rows: string[][] = []
  let cell = ''
  let row: string[] = []
  let quoted = false
  for (let index = 0; index < text.length; index += 1) {
    const character = text[index]
    if (character === '"' && quoted && text[index + 1] === '"') { cell += character; index += 1; continue }
    if (character === '"') { quoted = !quoted; continue }
    if (character === ',' && !quoted) { row = [...row, cell]; cell = ''; continue }
    if ((character === '\n' || character === '\r') && !quoted) {
      if (character === '\r' && text[index + 1] === '\n') index += 1
      rows.push([...row, cell]); row = []; cell = ''; continue
    }
    cell += character
  }
  if (cell || row.length) rows.push([...row, cell])
  return rows.filter(row => row.some(cell => cell.length > 0))
}

function toCsvCell(value: string) {
  return /[",\r\n]/.test(value) ? `"${value.replaceAll('"', '""')}"` : value
}

export function SheetsPage() {
  const { api } = useSession()
  const client = useQueryClient()
  const navigate = useNavigate()
  const [newName, setNewName] = useState('')
  const importRef = useRef<HTMLInputElement>(null)
  const sheets = useQuery({ queryKey: gridKey, queryFn: api.listSheets })
  const create = useMutation({
    mutationFn: api.createSheet,
    onSuccess: async response => {
      await client.invalidateQueries({ queryKey: gridKey })
      if (response.id) void navigate({ to: '/sheets/$sheetId', params: { sheetId: String(response.id) } })
    }
  })

  const createBlank = () => {
    const name = newName.trim() || 'Untitled spreadsheet'
    create.mutate({ name, description: '', headers_json: JSON.stringify(['Column 1', 'Column 2', 'Column 3']), data_json: JSON.stringify([['', '', '']]) })
    setNewName('')
  }

  const importCsv = async (file: File) => {
    const rows = parseCsv(await file.text())
    if (!rows.length) return
    const headers = rows[0].map(String)
    const data = rows.slice(1).map(row => headers.map((_, index) => String(row[index] ?? '')))
    create.mutate({ name: file.name.replace(/\.[^.]+$/, '') || 'Imported spreadsheet', description: 'Imported from CSV', headers_json: JSON.stringify(headers), data_json: JSON.stringify(data) })
  }

  return (
    <div className="page">
      <PageHeader title="Spreadsheets" description="Create, import, and collaborate on structured workspace data." />
      <section className="toolbar" aria-label="Spreadsheet actions">
        <input aria-label="New spreadsheet name" onChange={event => setNewName(event.target.value)} placeholder="New spreadsheet name" value={newName} />
        <button disabled={create.isPending} onClick={createBlank} type="button"><Plus aria-hidden="true" size={17} /> Create</button>
        <input accept=".csv,text/csv" className="visually-hidden" onChange={event => { const file = event.target.files?.[0]; if (file) void importCsv(file); event.target.value = '' }} ref={importRef} type="file" />
        <button className="secondary" onClick={() => importRef.current?.click()} type="button"><Upload aria-hidden="true" size={17} /> Import CSV</button>
      </section>
      {create.isError && <ErrorState message="The spreadsheet could not be created. Check the gateway and try again." />}
      {sheets.isPending && <LoadingState label="Loading spreadsheets..." />}
      {sheets.isError && <ErrorState />}
      {sheets.data && (sheets.data.sheets?.length ? (
        <div className="resource-list">
          {sheets.data.sheets.map(sheet => <SheetCard key={sheet.id} sheet={sheet} />)}
        </div>
      ) : <EmptyState><FilePlus2 aria-hidden="true" size={28} /><p>No spreadsheets yet.</p></EmptyState>)}
    </div>
  )
}

function SheetCard({ sheet }: { sheet: Sheet }) {
  const { api } = useSession()
  const client = useQueryClient()
  const remove = useMutation({ mutationFn: () => api.deleteSheet(sheet.id), onSuccess: () => client.invalidateQueries({ queryKey: gridKey }) })
  return (
    <article className="resource-row">
      <div><h2><Link to="/sheets/$sheetId" params={{ sheetId: String(sheet.id) }}>{sheet.name}</Link></h2><p>{sheet.description || 'No description'}</p><small>{sheet.row_count ?? parseGrid(sheet.data_json).length} rows, {sheet.col_count ?? parseStringArray(sheet.headers_json).length} columns</small></div>
      <button aria-label={`Delete ${sheet.name}`} className="icon-button danger" disabled={remove.isPending} onClick={() => { if (window.confirm(`Delete ${sheet.name}?`)) remove.mutate() }} title="Delete spreadsheet" type="button"><Trash2 aria-hidden="true" size={17} /></button>
    </article>
  )
}

export function SheetEditorPage() {
  const { sheetId } = useParams({ from: '/sheets/$sheetId' })
  const id = Number(sheetId)
  const { api } = useSession()
  const client = useQueryClient()
  const sheet = useQuery({ queryKey: ['sheet', id], queryFn: () => api.getSheet(id) })
  const [draft, setDraft] = useState<{ name: string; description: string; headers: string[]; rows: string[][] }>()
  const parentRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    const source = sheet.data?.spreadsheet
    if (source) setDraft({ name: source.name, description: source.description ?? '', headers: parseStringArray(source.headers_json), rows: parseGrid(source.data_json) })
  }, [sheet.data])
  const rows = draft?.rows ?? []
  const virtualRows = useVirtualizer({ count: rows.length, getScrollElement: () => parentRef.current, estimateSize: () => 42, overscan: 8 })
  const save = useMutation({
    mutationFn: () => draft ? api.updateSheet(id, { name: draft.name.trim(), description: draft.description.trim(), headers_json: JSON.stringify(draft.headers), data_json: JSON.stringify(draft.rows) }) : Promise.reject(new Error('No draft')),
    onSuccess: () => client.invalidateQueries({ queryKey: ['sheet', id] })
  })
  const updateCell = (rowIndex: number, columnIndex: number, value: string) => setDraft(current => !current ? current : ({ ...current, rows: current.rows.map((row, index) => index === rowIndex ? row.map((cell, cellIndex) => cellIndex === columnIndex ? value : cell) : row) }))
  const download = () => {
    if (!draft) return
    const csv = [draft.headers, ...draft.rows].map(row => row.map(toCsvCell).join(',')).join('\r\n')
    const link = document.createElement('a')
    link.href = URL.createObjectURL(new Blob([csv], { type: 'text/csv;charset=utf-8' }))
    link.download = `${draft.name || 'spreadsheet'}.csv`
    link.click()
    URL.revokeObjectURL(link.href)
  }
  if (sheet.isPending || !draft) return <div className="page"><LoadingState label="Loading spreadsheet..." /></div>
  if (sheet.isError) return <div className="page"><ErrorState message="The spreadsheet could not be loaded." /></div>
  return (
    <div className="page editor-page">
      <PageHeader title={draft.name || 'Spreadsheet'} description={sheet.data.cache_source ? `Source: ${sheet.data.cache_source}` : 'Edit cells directly; updates are saved explicitly.'} actions={<><button className="secondary" onClick={download} type="button"><Download aria-hidden="true" size={17} /> Export CSV</button><button disabled={save.isPending || !draft.name.trim()} onClick={() => save.mutate()} type="button"><Save aria-hidden="true" size={17} /> {save.isPending ? 'Saving...' : 'Save'}</button></>} />
      <div className="editor-meta"><input aria-label="Spreadsheet name" onChange={event => setDraft(current => current && ({ ...current, name: event.target.value }))} value={draft.name} /><input aria-label="Spreadsheet description" onChange={event => setDraft(current => current && ({ ...current, description: event.target.value }))} value={draft.description} /></div>
      {save.isError && <ErrorState message="Save failed. Your local edits are still present." />}
      <div className="sheet-grid" ref={parentRef}>
        <div className="grid-row grid-header"><span>#</span>{draft.headers.map((header, index) => <input aria-label={`Column ${index + 1}`} key={index} onChange={event => setDraft(current => current && ({ ...current, headers: current.headers.map((item, itemIndex) => itemIndex === index ? event.target.value : item) }))} value={header} />)}</div>
        <div style={{ height: `${virtualRows.getTotalSize()}px`, position: 'relative' }}>
          {virtualRows.getVirtualItems().map(virtualRow => { const row = rows[virtualRow.index] ?? []; return <div className="grid-row" key={virtualRow.key} style={{ height: `${virtualRow.size}px`, position: 'absolute', top: 0, transform: `translateY(${virtualRow.start}px)` }}><span>{virtualRow.index + 1}</span>{draft.headers.map((_, columnIndex) => <input aria-label={`Row ${virtualRow.index + 1} column ${columnIndex + 1}`} key={columnIndex} onChange={event => updateCell(virtualRow.index, columnIndex, event.target.value)} value={row[columnIndex] ?? ''} />)}</div> })}
        </div>
      </div>
    </div>
  )
}

export function FilesPage() {
  const { api } = useSession()
  const client = useQueryClient()
  const uploadRef = useRef<HTMLInputElement>(null)
  const [folderName, setFolderName] = useState('')
  const files = useQuery({ queryKey: fileKey, queryFn: api.listFiles })
  const refresh = () => client.invalidateQueries({ queryKey: fileKey })
  const upload = useMutation({ mutationFn: api.uploadFile, onSuccess: refresh })
  const folder = useMutation({ mutationFn: () => api.createFolder(folderName.trim()), onSuccess: () => { setFolderName(''); return refresh() } })
  const remove = useMutation({ mutationFn: api.deleteFile, onSuccess: refresh })
  return <div className="page"><PageHeader title="Files" description="Upload and organise file resources; the gateway enforces the 50 MB upload limit." />
    <section className="toolbar"><input aria-label="Folder name" onChange={event => setFolderName(event.target.value)} placeholder="New folder" value={folderName} /><button className="secondary" disabled={!folderName.trim() || folder.isPending} onClick={() => folder.mutate()} type="button"><FolderPlus aria-hidden="true" size={17} /> Create folder</button><input className="visually-hidden" onChange={event => { const file = event.target.files?.[0]; if (file) upload.mutate(file); event.target.value = '' }} ref={uploadRef} type="file" /><button disabled={upload.isPending} onClick={() => uploadRef.current?.click()} type="button"><Upload aria-hidden="true" size={17} /> {upload.isPending ? 'Uploading...' : 'Upload'}</button></section>
    {(upload.isError || folder.isError || remove.isError) && <ErrorState message="The file operation failed. Verify file size and server availability." />}
    {files.isPending && <LoadingState label="Loading files..." />}{files.isError && <ErrorState />}
    {files.data && (files.data.files?.length ? <div className="resource-list">{files.data.files.map(file => <article className="resource-row" key={file.id}><div><h2>{file.is_folder ? 'Folder' : 'File'}: {file.original_name}</h2><p>{file.is_folder ? 'Folder' : file.mime_type || 'Unknown format'}</p><small>{file.is_folder ? 'Container' : formatBytes(file.size)}</small></div><div className="row-actions">{!file.is_folder && <a aria-label={`Download ${file.original_name}`} className="icon-button" href={`/api/v1/files/${file.id}`} title="Download"><Download aria-hidden="true" size={17} /></a>}<button aria-label={`Delete ${file.original_name}`} className="icon-button danger" onClick={() => { if (window.confirm(`Delete ${file.original_name}?`)) remove.mutate(file.id) }} title="Delete" type="button"><Trash2 aria-hidden="true" size={17} /></button></div></article>)}</div> : <EmptyState><FilePlus2 aria-hidden="true" size={28} /><p>No files yet.</p></EmptyState>)}</div>
}

export function WorkspacesPage() {
  const { api } = useSession()
  const client = useQueryClient()
  const [name, setName] = useState('')
  const workspaces = useQuery({ queryKey: workspaceKey, queryFn: api.listWorkspaces })
  const create = useMutation({ mutationFn: () => api.createWorkspace(name.trim()), onSuccess: () => { setName(''); return client.invalidateQueries({ queryKey: workspaceKey }) } })
  const remove = useMutation({ mutationFn: api.deleteWorkspace, onSuccess: () => client.invalidateQueries({ queryKey: workspaceKey }) })
  return <div className="page"><PageHeader title="Workspaces" description="Use workspaces to keep team resources and membership boundaries clear." /><section className="toolbar"><input aria-label="Workspace name" onChange={event => setName(event.target.value)} placeholder="Workspace name" value={name} /><button disabled={!name.trim() || create.isPending} onClick={() => create.mutate()} type="button"><Plus aria-hidden="true" size={17} /> Create workspace</button></section>{(create.isError || remove.isError) && <ErrorState message="Workspace operation failed." />}{workspaces.isPending && <LoadingState label="Loading workspaces..." />}{workspaces.isError && <ErrorState />}{workspaces.data && (workspaces.data.workspaces?.length ? <div className="resource-list">{workspaces.data.workspaces.map(workspace => <article className="resource-row" key={workspace.id}><div><h2>{workspace.name}</h2><p>Owner ID: {workspace.owner_id}</p><small>{workspace.members?.length ?? 0} members</small></div><button aria-label={`Delete ${workspace.name}`} className="icon-button danger" onClick={() => { if (window.confirm(`Delete ${workspace.name}?`)) remove.mutate(workspace.id) }} title="Delete workspace" type="button"><Trash2 aria-hidden="true" size={17} /></button></article>)}</div> : <EmptyState><p>No workspaces yet.</p></EmptyState>)}</div>
}
