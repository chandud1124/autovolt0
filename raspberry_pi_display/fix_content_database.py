#!/usr/bin/env python3
"""
Quick fix script for Raspberry Pi display content issue
Run this on the Raspberry Pi to fix the is_active column issue
"""

import sqlite3
import os
import sys

def fix_database():
    """Fix the is_active column in the content database"""
    
    db_path = '/var/lib/raspberry-display/content.db'
    
    # Check if database exists
    if not os.path.exists(db_path):
        print(f"❌ Database not found at: {db_path}")
        print("   Make sure display-client service has run at least once")
        return False
    
    try:
        # Connect to database
        conn = sqlite3.connect(db_path)
        cursor = conn.cursor()
        
        # Check current content
        cursor.execute("SELECT id, title, is_active, schedule_start, schedule_end FROM content")
        rows = cursor.fetchall()
        
        print("=== CURRENT CONTENT IN DATABASE ===\n")
        print(f"Total content items: {len(rows)}\n")
        
        if len(rows) == 0:
            print("⚠️  No content found in database!")
            print("   The display client may not have fetched content yet.")
            print("   Wait 30 seconds and try again.")
            conn.close()
            return False
        
        for row in rows:
            content_id, title, is_active, schedule_start, schedule_end = row
            print(f"ID: {content_id}")
            print(f"Title: {title}")
            print(f"is_active: {is_active}")
            print(f"schedule_start: {schedule_start}")
            print(f"schedule_end: {schedule_end}")
            print()
        
        # Fix is_active for all content
        print("=== FIXING is_active COLUMN ===\n")
        cursor.execute("UPDATE content SET is_active = 1")
        rows_affected = cursor.rowcount
        conn.commit()
        
        print(f"✅ Updated {rows_affected} rows")
        print()
        
        # Verify fix
        cursor.execute("SELECT id, title, is_active FROM content")
        rows = cursor.fetchall()
        
        print("=== VERIFICATION ===\n")
        all_active = True
        for row in rows:
            content_id, title, is_active = row
            status = "✅" if is_active == 1 else "❌"
            print(f"{status} {title}: is_active = {is_active}")
            if is_active != 1:
                all_active = False
        
        conn.close()
        
        if all_active:
            print("\n✅ All content is now active!")
            print("\nNext steps:")
            print("  sudo systemctl restart display-client")
            return True
        else:
            print("\n⚠️  Some content is still not active")
            return False
            
    except sqlite3.Error as e:
        print(f"❌ Database error: {e}")
        return False
    except Exception as e:
        print(f"❌ Error: {e}")
        return False

if __name__ == "__main__":
    print("Raspberry Pi Display Content Fix")
    print("=" * 50)
    print()
    
    if os.geteuid() != 0:
        print("⚠️  This script should be run as root (use sudo)")
        print()
    
    success = fix_database()
    
    if success:
        print("\n✅ Fix completed successfully!")
        sys.exit(0)
    else:
        print("\n❌ Fix failed!")
        sys.exit(1)