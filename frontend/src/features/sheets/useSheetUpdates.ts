import { useEffect } from 'react'

interface UpdateMessage {
  room?: unknown
  type?: unknown
  data?: { user?: unknown }
}

function isUpdateForSheet(message: unknown, room: string): message is UpdateMessage {
  return typeof message === 'object' && message !== null
    && (message as UpdateMessage).room === room
    && (message as UpdateMessage).type === 'sheet.updated'
}

export function useSheetUpdates(sheetId: string | number, onRemoteUpdate: (username?: string) => void) {
  useEffect(() => {
    const room = `sheet:${sheetId}`
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
    const socket = new WebSocket(`${protocol}//${window.location.host}/api/v1/ws`)

    socket.addEventListener('open', () => {
      socket.send(JSON.stringify({ action: 'subscribe', room }))
    })
    socket.addEventListener('message', event => {
      try {
        const message: unknown = JSON.parse(String(event.data))
        if (isUpdateForSheet(message, room)) {
          const username = typeof message.data?.user === 'string' ? message.data.user : undefined
          onRemoteUpdate(username)
        }
      } catch {
        // Ignore malformed or unrelated messages from the realtime channel.
      }
    })

    return () => socket.close()
  }, [onRemoteUpdate, sheetId])
}
