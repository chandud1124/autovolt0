# Ticket System - Visual Process Flowchart

## 🎯 Complete Process After Creating a Ticket

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         USER CREATES TICKET                              │
│  ┌──────────────────────────────────────────────────────────────┐      │
│  │ 1. Fill form (title, description, category, priority)        │      │
│  │ 2. Type @ to mention users                                   │      │
│  │ 3. Select: John (IT Admin), Jane (Network Admin)            │      │
│  │ 4. Click "Create Ticket"                                     │      │
│  └──────────────────────────────────────────────────────────────┘      │
└───────────────────────────────┬─────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    FRONTEND → BACKEND (API CALL)                         │
│  ┌──────────────────────────────────────────────────────────────┐      │
│  │ POST /api/tickets                                            │      │
│  │ {                                                            │      │
│  │   title: "Network Issue in Room 101",                       │      │
│  │   description: "WiFi not working",                          │      │
│  │   category: "network_issue",                                │      │
│  │   priority: "high",                                         │      │
│  │   mentionedUsers: ["USER_ID_JOHN", "USER_ID_JANE"]        │      │
│  │ }                                                            │      │
│  └──────────────────────────────────────────────────────────────┘      │
└───────────────────────────────┬─────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                     BACKEND: createTicket() Controller                   │
│  ┌──────────────────────────────────────────────────────────────┐      │
│  │ Step 1: Validate Request                                     │      │
│  │   ✓ Check required fields (title, description, category)    │      │
│  │   ✓ Validate category enum                                  │      │
│  │   ✓ Get authenticated user                                  │      │
│  └──────────────────────────────────────────────────────────────┘      │
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────┐      │
│  │ Step 2: Create Ticket Document                               │      │
│  │   • Generate ticketId: TKT-2025-0123                        │      │
│  │   • Set status: "open"                                       │      │
│  │   • Add creator reference                                    │      │
│  │   • Store mentionedUsers array                               │      │
│  │   • Add initial comment: "Ticket created"                    │      │
│  │   • Save to MongoDB ✓                                        │      │
│  └──────────────────────────────────────────────────────────────┘      │
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────┐      │
│  │ Step 3: Populate References                                  │      │
│  │   • Populate createdBy user data                            │      │
│  │   • Populate mentionedUsers data                            │      │
│  │   • Result: Full user objects with name, email, role        │      │
│  └──────────────────────────────────────────────────────────────┘      │
└───────────────────────────────┬─────────────────────────────────────────┘
                                │
                ┌───────────────┴───────────────┐
                ▼                               ▼
┌─────────────────────────────┐   ┌─────────────────────────────┐
│   FOR JOHN (Mentioned)      │   │   FOR JANE (Mentioned)      │
│  ┌────────────────────────┐ │   │  ┌────────────────────────┐ │
│  │ Create Notification:   │ │   │  │ Create Notification:   │ │
│  │ ─────────────────────  │ │   │  │ ─────────────────────  │ │
│  │ recipient: John        │ │   │  │ recipient: Jane        │ │
│  │ type: ticket_mention   │ │   │  │ type: ticket_mention   │ │
│  │ title: "You were       │ │   │  │ title: "You were       │ │
│  │   mentioned..."        │ │   │  │   mentioned..."        │ │
│  │ priority: high         │ │   │  │ priority: high         │ │
│  │ isRead: false          │ │   │  │ isRead: false          │ │
│  │ Save to MongoDB ✓      │ │   │  │ Save to MongoDB ✓      │ │
│  └────────────────────────┘ │   │  └────────────────────────┘ │
│                             │   │                             │
│  ┌────────────────────────┐ │   │  ┌────────────────────────┐ │
│  │ Emit Socket.IO Event:  │ │   │  │ Emit Socket.IO Event:  │ │
│  │ ─────────────────────  │ │   │  │ ─────────────────────  │ │
│  │ io.to(John's room)     │ │   │  │ io.to(Jane's room)     │ │
│  │   .emit('notification')│ │   │  │   .emit('notification')│ │
│  │                        │ │   │  │                        │ │
│  │ ✓ Real-time delivery   │ │   │  │ ✓ Real-time delivery   │ │
│  └────────────────────────┘ │   │  └────────────────────────┘ │
└─────────────┬───────────────┘   └─────────────┬───────────────┘
              │                                 │
              └────────────┬────────────────────┘
                           ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    BACKEND → FRONTEND (Response)                         │
│  ┌──────────────────────────────────────────────────────────────┐      │
│  │ HTTP 201 Created                                             │      │
│  │ {                                                            │      │
│  │   success: true,                                            │      │
│  │   data: {                                                   │      │
│  │     ticketId: "TKT-2025-0123",                             │      │
│  │     title: "Network Issue in Room 101",                    │      │
│  │     status: "open",                                        │      │
│  │     mentionedUsers: [                                      │      │
│  │       { name: "John Smith", email: "john@...", role: ... },│      │
│  │       { name: "Jane Doe", email: "jane@...", role: ... }   │      │
│  │     ],                                                     │      │
│  │     createdAt: "2025-01-15T10:00:00Z"                     │      │
│  │   }                                                        │      │
│  │ }                                                          │      │
│  └──────────────────────────────────────────────────────────────┘      │
└───────────────────────────────┬─────────────────────────────────────────┘
                                │
                ┌───────────────┼───────────────┐
                ▼               ▼               ▼
┌─────────────────────┐ ┌─────────────────────┐ ┌─────────────────────┐
│   CREATOR'S         │ │   JOHN'S            │ │   JANE'S            │
│   BROWSER           │ │   BROWSER           │ │   BROWSER           │
│  ┌────────────────┐ │ │  ┌────────────────┐ │ │  ┌────────────────┐ │
│  │ Toast Appears: │ │ │  │ Socket.IO      │ │ │  │ Socket.IO      │ │
│  │ ✓ "Ticket      │ │ │  │ receives event │ │ │  │ receives event │ │
│  │   Created"     │ │ │  │                │ │ │  │                │ │
│  │                │ │ │  │ Bell icon: 🔔[1]│ │ │  │ Bell icon: 🔔[1]│ │
│  │ Dialog closes  │ │ │  │                │ │ │  │                │ │
│  │                │ │ │  │ Toast shows:   │ │ │  │ Toast shows:   │ │
│  │ Ticket list    │ │ │  │ "You were      │ │ │  │ "You were      │ │
│  │ refreshes      │ │ │  │  mentioned..." │ │ │  │  mentioned..." │ │
│  │                │ │ │  │                │ │ │  │                │ │
│  │ New ticket     │ │ │  │ Notification   │ │ │  │ Notification   │ │
│  │ visible at top │ │ │  │ added to list  │ │ │  │ added to list  │ │
│  └────────────────┘ │ │  └────────────────┘ │ │  └────────────────┘ │
└─────────────────────┘ └─────────────────────┘ └─────────────────────┘

═══════════════════════════════════════════════════════════════════════════

                    ⏱️ TIME: 10:05 AM (5 minutes later)

═══════════════════════════════════════════════════════════════════════════

┌─────────────────────────────────────────────────────────────────────────┐
│                     JOHN CLICKS NOTIFICATION                             │
│  ┌──────────────────────────────────────────────────────────────┐      │
│  │ 1. Clicks notification bell 🔔[1]                            │      │
│  │ 2. Sees: "You were mentioned in Network Issue in Room 101"  │      │
│  │ 3. Clicks "View Ticket" button                              │      │
│  └──────────────────────────────────────────────────────────────┘      │
└───────────────────────────────┬─────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    FRONTEND: Mark Notification as Read                   │
│  ┌──────────────────────────────────────────────────────────────┐      │
│  │ PUT /api/auth/notifications/{notificationId}/read            │      │
│  │                                                              │      │
│  │ • isRead: true                                              │      │
│  │ • readAt: current timestamp                                 │      │
│  │ • Bell count decreases: 🔔[0]                               │      │
│  └──────────────────────────────────────────────────────────────┘      │
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────┐      │
│  │ Navigate to /support page                                    │      │
│  │ • Ticket TKT-2025-0123 visible in list                      │      │
│  │ • John can click to see full details                        │      │
│  │ • Can add comments, track progress                          │      │
│  └──────────────────────────────────────────────────────────────┘      │
└─────────────────────────────────────────────────────────────────────────┘

═══════════════════════════════════════════════════════════════════════════

                    ⏱️ TIME: 10:10 AM (10 minutes later)

═══════════════════════════════════════════════════════════════════════════

┌─────────────────────────────────────────────────────────────────────────┐
│                    ADMIN CHANGES TICKET STATUS                           │
│  ┌──────────────────────────────────────────────────────────────┐      │
│  │ 1. Admin opens ticket TKT-2025-0123                         │      │
│  │ 2. Clicks "Start Processing" button                         │      │
│  │ 3. Status changes: Open → In Progress                       │      │
│  └──────────────────────────────────────────────────────────────┘      │
└───────────────────────────────┬─────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                  BACKEND: updateTicket() Controller                      │
│  ┌──────────────────────────────────────────────────────────────┐      │
│  │ Step 1: Detect Status Change                                 │      │
│  │   oldStatus = "open"                                         │      │
│  │   newStatus = "in_progress"                                 │      │
│  │   statusChanged = true ✓                                    │      │
│  └──────────────────────────────────────────────────────────────┘      │
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────┐      │
│  │ Step 2: Update Ticket                                        │      │
│  │   • ticket.status = "in_progress"                           │      │
│  │   • Add comment: "Status changed to In Progress"            │      │
│  │   • Save to MongoDB ✓                                       │      │
│  └──────────────────────────────────────────────────────────────┘      │
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────┐      │
│  │ Step 3: Build Notification Recipients                        │      │
│  │   usersToNotify = Set()                                     │      │
│  │   • Add: ticket.createdBy (Creator)                         │      │
│  │   • Add: ticket.assignedTo (if exists)                      │      │
│  │   • Add: ticket.mentionedUsers (John, Jane)                 │      │
│  │   • Remove: current admin (don't notify yourself)           │      │
│  │                                                             │      │
│  │   Result: [Creator, John, Jane]  ← 3 users to notify       │      │
│  └──────────────────────────────────────────────────────────────┘      │
└───────────────────────────────┬─────────────────────────────────────────┘
                                │
        ┌───────────────────────┼───────────────────────┐
        ▼                       ▼                       ▼
┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
│  FOR CREATOR    │    │  FOR JOHN       │    │  FOR JANE       │
│ ┌─────────────┐ │    │ ┌─────────────┐ │    │ ┌─────────────┐ │
│ │ Notification│ │    │ │ Notification│ │    │ │ Notification│ │
│ │ ───────────│ │    │ │ ───────────│ │    │ │ ───────────│ │
│ │ type:      │ │    │ │ type:      │ │    │ │ type:      │ │
│ │ status_    │ │    │ │ status_    │ │    │ │ status_    │ │
│ │ change     │ │    │ │ change     │ │    │ │ change     │ │
│ │            │ │    │ │            │ │    │ │            │ │
│ │ message:   │ │    │ │ message:   │ │    │ │ message:   │ │
│ │ "Admin     │ │    │ │ "Admin     │ │    │ │ "Admin     │ │
│ │ changed    │ │    │ │ changed    │ │    │ │ changed    │ │
│ │ from Open  │ │    │ │ from Open  │ │    │ │ from Open  │ │
│ │ to In      │ │    │ │ to In      │ │    │ │ to In      │ │
│ │ Progress"  │ │    │ │ Progress"  │ │    │ │ Progress"  │ │
│ │            │ │    │ │            │ │    │ │            │ │
│ │ Save ✓     │ │    │ │ Save ✓     │ │    │ │ Save ✓     │ │
│ │ Emit ✓     │ │    │ │ Emit ✓     │ │    │ │ Emit ✓     │ │
│ └─────────────┘ │    │ └─────────────┘ │    │ └─────────────┘ │
└─────────────────┘    └─────────────────┘    └─────────────────┘
        │                       │                       │
        └───────────────────────┼───────────────────────┘
                                ▼
┌─────────────────────────────────────────────────────────────────────────┐
│              ALL 3 USERS SEE NEW NOTIFICATION                            │
│  ┌──────────────────────────────────────────────────────────────┐      │
│  │ 🔔 Notification Bell: [1] for each user                      │      │
│  │                                                              │      │
│  │ "Ticket status updated to In Progress"                      │      │
│  │ "Admin changed status from Open to In Progress"             │      │
│  │                                                              │      │
│  │ [View Ticket] →                                             │      │
│  └──────────────────────────────────────────────────────────────┘      │
└─────────────────────────────────────────────────────────────────────────┘

═══════════════════════════════════════════════════════════════════════════

                           STATUS FLOW DIAGRAM

═══════════════════════════════════════════════════════════════════════════

    OPEN
     │
     │ (Admin: "Start Processing")
     │ ↓ Notify: Creator, John, Jane
     │
IN PROGRESS
     │
     │ (Admin: "Put On Hold")
     │ ↓ Notify: Creator, John, Jane
     │
  ON HOLD
     │
     │ (Admin: "Resume")
     │ ↓ Notify: Creator, John, Jane
     │
IN PROGRESS
     │
     │ (Admin: "Mark Resolved")
     │ ↓ Notify: Creator, John, Jane
     │
  RESOLVED
     │
     │ (Admin: "Close Ticket")
     │ ↓ Notify: Creator, John, Jane
     │
   CLOSED

Each status change triggers notifications to ALL involved users!

═══════════════════════════════════════════════════════════════════════════

                    DATABASE STATE DIAGRAM

═══════════════════════════════════════════════════════════════════════════

┌─────────────────────────────────────────────────────────────────────────┐
│                        MONGODB - tickets collection                      │
│  ┌──────────────────────────────────────────────────────────────┐      │
│  │ {                                                            │      │
│  │   ticketId: "TKT-2025-0123",                                │      │
│  │   title: "Network Issue in Room 101",                       │      │
│  │   status: "in_progress",                                    │      │
│  │   priority: "high",                                         │      │
│  │   createdBy: ObjectId("..."),          ← References User    │      │
│  │   mentionedUsers: [                                         │      │
│  │     ObjectId("USER_ID_JOHN"),          ← References User    │      │
│  │     ObjectId("USER_ID_JANE")           ← References User    │      │
│  │   ],                                                        │      │
│  │   comments: [                                               │      │
│  │     { message: "Ticket created", ... },                     │      │
│  │     { message: "Status changed to In Progress", ... }       │      │
│  │   ]                                                         │      │
│  │ }                                                           │      │
│  └──────────────────────────────────────────────────────────────┘      │
└─────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│                   MONGODB - notifications collection                     │
│  ┌──────────────────────────────────────────────────────────────┐      │
│  │ Notification 1: (ticket_mention for John)                   │      │
│  │ {                                                            │      │
│  │   recipient: ObjectId("USER_ID_JOHN"),                      │      │
│  │   type: "ticket_mention",                                   │      │
│  │   isRead: true,                                            │      │
│  │   readAt: ISODate("2025-01-15T10:05:00Z")                  │      │
│  │ }                                                           │      │
│  └──────────────────────────────────────────────────────────────┘      │
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────┐      │
│  │ Notification 2: (ticket_mention for Jane)                   │      │
│  │ {                                                            │      │
│  │   recipient: ObjectId("USER_ID_JANE"),                      │      │
│  │   type: "ticket_mention",                                   │      │
│  │   isRead: false,                                           │      │
│  │   readAt: null                                             │      │
│  │ }                                                           │      │
│  └──────────────────────────────────────────────────────────────┘      │
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────┐      │
│  │ Notification 3: (status_change for Creator)                 │      │
│  │ {                                                            │      │
│  │   recipient: ObjectId("USER_ID_CREATOR"),                   │      │
│  │   type: "ticket_status_change",                            │      │
│  │   message: "Admin changed status from Open to In Progress", │      │
│  │   isRead: false                                            │      │
│  │ }                                                           │      │
│  └──────────────────────────────────────────────────────────────┘      │
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────┐      │
│  │ Notification 4: (status_change for John)                    │      │
│  │ { ... same structure ... }                                   │      │
│  └──────────────────────────────────────────────────────────────┘      │
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────┐      │
│  │ Notification 5: (status_change for Jane)                    │      │
│  │ { ... same structure ... }                                   │      │
│  └──────────────────────────────────────────────────────────────┘      │
└─────────────────────────────────────────────────────────────────────────┘

═══════════════════════════════════════════════════════════════════════════

                        KEY TAKEAWAYS

═══════════════════════════════════════════════════════════════════════════

1. ✅ Ticket Created → Mentioned users notified IMMEDIATELY
2. ✅ Status Changed → All involved users notified (except admin)
3. ✅ Real-time delivery via Socket.IO
4. ✅ Persistent storage in MongoDB
5. ✅ Complete audit trail maintained
6. ✅ No notifications lost (offline users get them later)
7. ✅ Professional, transparent workflow

═══════════════════════════════════════════════════════════════════════════
```

## 📊 Summary of Complete Process

### **Timeline:**
```
T+0s    : User creates ticket with mentions
T+0.5s  : Ticket saved to database
T+0.7s  : Notifications created for John & Jane
T+0.8s  : Socket.IO events emitted
T+1s    : John & Jane see notification bells light up ✅
T+5min  : John clicks notification, views ticket ✅
T+10min : Admin changes status to "In Progress"
T+10min : Creator, John, Jane all notified ✅
T+2hr   : Admin resolves ticket
T+2hr   : All users notified of resolution ✅
```

### **Notification Count Over Time:**
```
10:00 AM - Ticket Created
           John: 🔔[1]  Jane: 🔔[1]  Creator: 🔔[0]

10:05 AM - John views notification
           John: 🔔[0]  Jane: 🔔[1]  Creator: 🔔[0]

10:10 AM - Status changed to In Progress
           John: 🔔[1]  Jane: 🔔[2]  Creator: 🔔[1]

11:30 AM - Status changed to Resolved
           John: 🔔[2]  Jane: 🔔[3]  Creator: 🔔[2]
```

### **Who Gets Notified:**
```
┌─────────────────────┬─────────┬──────────┬──────────┬───────┐
│ Event               │ Creator │ Assignee │ Mentions │ Admin │
├─────────────────────┼─────────┼──────────┼──────────┼───────┤
│ Ticket Created      │    ✗    │    ✗     │    ✅    │   ✗   │
│ Status Changed      │    ✅    │    ✅    │    ✅    │   ✗   │
│ Ticket Assigned     │    ✗    │    ✅    │    ✗     │   ✗   │
│ Comment Added*      │    ✅    │    ✅    │    ✅    │   ✗   │
└─────────────────────┴─────────┴──────────┴──────────┴───────┘

* Comment notifications = Future enhancement
✅ = Receives notification
✗ = Does NOT receive notification
```

---

**This flowchart shows the complete process that happens after creating a ticket with mentions. Every user stays informed in real-time!** 🎉
