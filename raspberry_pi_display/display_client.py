#!/usr/bin/env python3
"""
Raspberry Pi Display Client for AIMS Smart Class System
Displays notices and content from the backend server on connected displays.
"""

import os
import sys
import time
import json
import requests
import logging
import sqlite3
import hashlib
import shutil
import io
from datetime import datetime, timedelta
from typing import Dict, List, Optional, Any
import pygame
import pygame.freetype
from pygame.locals import *
import threading
import signal
from dotenv import load_dotenv
import urllib.request
import paho.mqtt.client as mqtt

# Load environment variables from .env file
load_dotenv()

# Configuration
class Config:
    # Server settings
    SERVER_URL = os.getenv('SERVER_URL', 'http://localhost:3001')
    BOARD_ID = os.getenv('BOARD_ID', '')  # Must be set
    API_KEY = os.getenv('API_KEY', '')    # Optional API key for authentication

    # Display settings
    DISPLAY_WIDTH = int(os.getenv('DISPLAY_WIDTH', '1920'))
    DISPLAY_HEIGHT = int(os.getenv('DISPLAY_HEIGHT', '1080'))
    FULLSCREEN = os.getenv('FULLSCREEN', 'true').lower() == 'true'

    # Update intervals (seconds)
    CONTENT_UPDATE_INTERVAL = int(os.getenv('CONTENT_UPDATE_INTERVAL', '30'))
    STATUS_UPDATE_INTERVAL = int(os.getenv('STATUS_UPDATE_INTERVAL', '60'))

    # Display settings
    FONT_SIZE_TITLE = int(os.getenv('FONT_SIZE_TITLE', '48'))
    FONT_SIZE_CONTENT = int(os.getenv('FONT_SIZE_CONTENT', '32'))
    FONT_SIZE_FOOTER = int(os.getenv('FONT_SIZE_FOOTER', '24'))

    # MQTT Configuration
    MQTT_BROKER = os.getenv('MQTT_BROKER', 'localhost')
    MQTT_PORT = int(os.getenv('MQTT_PORT', '1883'))
    MQTT_TOPIC = os.getenv('MQTT_TOPIC', 'notices/published')

    # Colors (RGB)
    BG_COLOR = tuple(map(int, os.getenv('BG_COLOR', '0,0,0').split(',')))
    TEXT_COLOR = tuple(map(int, os.getenv('TEXT_COLOR', '255,255,255').split(',')))
    TITLE_COLOR = tuple(map(int, os.getenv('TITLE_COLOR', '255,255,0').split(',')))

    # Local storage settings
    STORAGE_DIR = None  # Will be set in LocalStorageManager
    CONTENT_DIR = None
    DATABASE_PATH = None
    CACHE_DIR = None
    LOGS_DIR = None

    # Content sync settings
    MAX_CACHE_AGE_DAYS = int(os.getenv('MAX_CACHE_AGE_DAYS', '7'))
    MAX_STORAGE_SIZE_MB = int(os.getenv('MAX_STORAGE_SIZE_MB', '1024'))  # 1GB default


class LocalStorageManager:
    """Manages local storage of content and metadata"""

    def __init__(self):
        self.ensure_directories()
        self.init_database()

    def ensure_directories(self):
        """Create necessary directories if they don't exist"""
        from pathlib import Path
        Config.STORAGE_DIR = Path(os.getenv('STORAGE_DIR', '/var/lib/raspberry-display'))
        Config.CONTENT_DIR = Config.STORAGE_DIR / 'content'
        Config.CACHE_DIR = Config.STORAGE_DIR / 'cache'
        Config.DATABASE_PATH = Config.STORAGE_DIR / 'content.db'
        Config.LOGS_DIR = Path(os.getenv('LOGS_DIR', '/var/log/raspberry-display'))
        
        Config.STORAGE_DIR.mkdir(parents=True, exist_ok=True)
        Config.CONTENT_DIR.mkdir(parents=True, exist_ok=True)
        Config.CACHE_DIR.mkdir(parents=True, exist_ok=True)
        Config.LOGS_DIR.mkdir(parents=True, exist_ok=True)

    def init_database(self):
        """Initialize SQLite database for content metadata"""
        with sqlite3.connect(Config.DATABASE_PATH) as conn:
            conn.execute('''
                CREATE TABLE IF NOT EXISTS content (
                    id TEXT PRIMARY KEY,
                    notice_id TEXT,
                    title TEXT,
                    content TEXT,
                    content_type TEXT,
                    priority INTEGER DEFAULT 0,
                    schedule_type TEXT DEFAULT 'fixed',  -- 'fixed', 'recurring', 'permanent'
                    schedule_start DATETIME,
                    schedule_end DATETIME,
                    display_duration INTEGER DEFAULT 60,
                    attachments TEXT,  -- JSON array of attachment metadata
                    downloaded_at DATETIME DEFAULT CURRENT_TIMESTAMP,
                    last_accessed DATETIME DEFAULT CURRENT_TIMESTAMP,
                    is_active BOOLEAN DEFAULT 1,
                    is_recurring BOOLEAN DEFAULT 0
                )
            ''')

            conn.execute('''
                CREATE TABLE IF NOT EXISTS attachments (
                    id TEXT PRIMARY KEY,
                    content_id TEXT,
                    filename TEXT,
                    original_name TEXT,
                    mimetype TEXT,
                    size INTEGER,
                    local_path TEXT,
                    remote_url TEXT,
                    downloaded_at DATETIME DEFAULT CURRENT_TIMESTAMP,
                    FOREIGN KEY (content_id) REFERENCES content (id)
                )
            ''')

            conn.execute('''
                CREATE INDEX IF NOT EXISTS idx_content_active ON content (is_active, schedule_start, schedule_end)
            ''')

            conn.execute('''
                CREATE INDEX IF NOT EXISTS idx_content_priority ON content (priority DESC)
            ''')

            conn.execute('''
                CREATE INDEX IF NOT EXISTS idx_content_recurring ON content (is_recurring, last_accessed)
            ''')

    def store_content(self, content_data: Dict[str, Any]) -> bool:
        """Store content metadata and download attachments"""
        try:
            # Get content from both scheduledContent (new) and boardNotices (legacy)
            scheduled_content = content_data.get('scheduledContent', [])
            board_notices = content_data.get('boardNotices', [])
            group_content = content_data.get('groupContent', [])
            
            # Prioritize scheduledContent over boardNotices
            notices = scheduled_content + board_notices + group_content

            with sqlite3.connect(Config.DATABASE_PATH) as conn:
                for notice in notices:
                    content_id = str(notice.get('_id', ''))
                    if not content_id:
                        continue

                    # Check if content already exists and is up to date
                    existing = conn.execute(
                        'SELECT downloaded_at FROM content WHERE id = ?',
                        (content_id,)
                    ).fetchone()

                    notice_updated = notice.get('updatedAt', notice.get('createdAt', ''))
                    if existing and notice_updated:
                        # Skip if content hasn't changed
                        continue

                    # Determine if content is recurring
                    schedule = notice.get('schedule', {})
                    schedule_type = schedule.get('type', 'fixed')
                    is_recurring = schedule_type in ['recurring', 'permanent', 'always'] or not schedule.get('endDate')
                    
                    # Get priority from either priority field or type field
                    priority = notice.get('priority', 0)
                    if isinstance(priority, str):
                        priority_map = {'urgent': 10, 'high': 8, 'medium': 5, 'low': 3}
                        priority = priority_map.get(priority.lower(), 5)
                    
                    # Store content metadata
                    conn.execute('''
                        INSERT OR REPLACE INTO content
                        (id, notice_id, title, content, content_type, priority,
                         schedule_type, schedule_start, schedule_end, display_duration, 
                         attachments, is_recurring)
                        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                    ''', (
                        content_id,
                        content_id,
                        notice.get('title', ''),
                        notice.get('content', ''),
                        notice.get('contentType', notice.get('type', 'text')),
                        priority,
                        schedule_type,
                        schedule.get('startDate'),
                        schedule.get('endDate'),
                        notice.get('duration', schedule.get('duration', 60)),
                        json.dumps(notice.get('attachments', [])),
                        1 if is_recurring else 0
                    ))

                    # Download attachments
                    self._download_attachments(content_id, notice.get('attachments', []))

                conn.commit()

            logging.info(f"Stored {len(notices)} content items locally ({len(scheduled_content)} scheduled, {len(board_notices)} notices)")
            return True

        except Exception as e:
            logging.error(f"Error storing content: {e}")
            return False

    def _download_attachments(self, content_id: str, attachments: List[Dict]):
        """Download and store attachments locally"""
        for attachment in attachments:
            try:
                remote_url = attachment.get('url', '')
                if not remote_url:
                    continue

                # Make URL absolute if it's relative
                if not remote_url.startswith('http'):
                    remote_url = f"{Config.SERVER_URL}{remote_url}"

                # Generate local filename
                url_hash = hashlib.md5(remote_url.encode()).hexdigest()
                ext = os.path.splitext(attachment.get('originalName', ''))[1] or '.bin'
                local_filename = f"{url_hash}{ext}"
                local_path = Config.CONTENT_DIR / local_filename

                # Skip if already downloaded
                if local_path.exists():
                    logging.debug(f"Attachment already exists: {local_filename}")
                    # Still update database record
                    with sqlite3.connect(Config.DATABASE_PATH) as conn:
                        conn.execute('''
                            INSERT OR REPLACE INTO attachments
                            (id, content_id, filename, original_name, mimetype, size, local_path, remote_url)
                            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                        ''', (
                            f"{content_id}_{attachment.get('filename', '')}",
                            content_id,
                            attachment.get('filename', ''),
                            attachment.get('originalName', ''),
                            attachment.get('mimeType', attachment.get('mimetype', '')),
                            attachment.get('size', 0),
                            str(local_path),
                            remote_url
                        ))
                    continue

                # Download file
                logging.info(f"Downloading attachment from: {remote_url}")
                response = requests.get(remote_url, timeout=30)
                response.raise_for_status()

                # Save file
                with open(local_path, 'wb') as f:
                    f.write(response.content)

                # Store attachment metadata
                with sqlite3.connect(Config.DATABASE_PATH) as conn:
                    conn.execute('''
                        INSERT OR REPLACE INTO attachments
                        (id, content_id, filename, original_name, mimetype, size, local_path, remote_url)
                        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                    ''', (
                        f"{content_id}_{attachment.get('filename', '')}",
                        content_id,
                        attachment.get('filename', ''),
                        attachment.get('originalName', ''),
                        attachment.get('mimeType', attachment.get('mimetype', '')),
                        attachment.get('size', 0),
                        str(local_path),
                        remote_url
                    ))

                logging.info(f"Downloaded attachment: {attachment.get('originalName', '')} ({len(response.content)} bytes)")

            except Exception as e:
                logging.error(f"Error downloading attachment {attachment.get('originalName', '')}: {e}")

    def get_active_content(self) -> List[Dict[str, Any]]:
        """Get currently active content based on schedule"""
        try:
            now = datetime.now()

            with sqlite3.connect(Config.DATABASE_PATH) as conn:
                # Get content that should be active now
                rows = conn.execute('''
                    SELECT * FROM content
                    WHERE is_active = 1
                    AND (schedule_start IS NULL OR datetime(schedule_start) <= datetime(?))
                    AND (schedule_end IS NULL OR datetime(schedule_end) >= datetime(?))
                    ORDER BY priority DESC, downloaded_at DESC
                ''', (now.isoformat(), now.isoformat())).fetchall()

                content_list = []
                for row in rows:
                    content = {
                        '_id': row[0],
                        'title': row[2],
                        'content': row[3],
                        'contentType': row[4],
                        'priority': row[5],
                        'schedule': {
                            'type': row[6],
                            'startDate': row[7],
                            'endDate': row[8],
                            'duration': row[9]
                        },
                        'attachments': json.loads(row[10]) if row[10] else [],
                        'is_recurring': bool(row[13])
                    }

                    # Add local attachment paths
                    content['local_attachments'] = self._get_local_attachments(row[0])
                    content_list.append(content)

                return content_list

        except Exception as e:
            logging.error(f"Error getting active content: {e}")
            return []

    def _get_local_attachments(self, content_id: str) -> List[Dict]:
        """Get local attachment information for content"""
        try:
            with sqlite3.connect(Config.DATABASE_PATH) as conn:
                rows = conn.execute(
                    'SELECT filename, original_name, mimetype, size, local_path FROM attachments WHERE content_id = ?',
                    (content_id,)
                ).fetchall()

                return [{
                    'filename': row[0],
                    'originalName': row[1],
                    'mimetype': row[2],
                    'size': row[3],
                    'localPath': row[4]
                } for row in rows]

        except Exception as e:
            logging.error(f"Error getting local attachments: {e}")
            return []

    def cleanup_expired_content(self):
        """Remove expired limited-time content immediately"""
        try:
            now = datetime.now()

            with sqlite3.connect(Config.DATABASE_PATH) as conn:
                # Find expired limited-time content (not recurring and end date has passed)
                expired_content = conn.execute('''
                    SELECT id FROM content 
                    WHERE is_active = 1 
                    AND is_recurring = 0 
                    AND schedule_end IS NOT NULL 
                    AND datetime(schedule_end) < datetime(?)
                ''', (now.isoformat(),)).fetchall()

                for (content_id,) in expired_content:
                    # Remove attachments
                    attachments = conn.execute(
                        'SELECT local_path FROM attachments WHERE content_id = ?',
                        (content_id,)
                    ).fetchall()

                    for (local_path,) in attachments:
                        try:
                            os.unlink(local_path)
                        except Exception as e:
                            logging.warning(f"Could not remove file {local_path}: {e}")

                    # Remove from database
                    conn.execute('DELETE FROM attachments WHERE content_id = ?', (content_id,))
                    conn.execute('DELETE FROM content WHERE id = ?', (content_id,))
                    
                    logging.info(f"Removed expired content: {content_id}")

                conn.commit()

        except Exception as e:
            logging.error(f"Error during expired content cleanup: {e}")

    def cleanup_old_recurring_content(self):
        """Remove recurring content that hasn't been accessed for 7 days"""
        try:
            cutoff_date = datetime.now() - timedelta(days=7)

            with sqlite3.connect(Config.DATABASE_PATH) as conn:
                # Find recurring content not accessed for 7 days
                old_recurring = conn.execute('''
                    SELECT id FROM content 
                    WHERE is_recurring = 1 
                    AND datetime(last_accessed) < datetime(?)
                ''', (cutoff_date.isoformat(),)).fetchall()

                for (content_id,) in old_recurring:
                    # Remove attachments
                    attachments = conn.execute(
                        'SELECT local_path FROM attachments WHERE content_id = ?',
                        (content_id,)
                    ).fetchall()

                    for (local_path,) in attachments:
                        try:
                            os.unlink(local_path)
                        except Exception as e:
                            logging.warning(f"Could not remove file {local_path}: {e}")

                    # Remove from database
                    conn.execute('DELETE FROM attachments WHERE content_id = ?', (content_id,))
                    conn.execute('DELETE FROM content WHERE id = ?', (content_id,))
                    
                    logging.info(f"Removed old recurring content: {content_id}")

                conn.commit()

        except Exception as e:
            logging.error(f"Error during recurring content cleanup: {e}")

    def update_last_accessed(self, content_id: str):
        """Update the last accessed timestamp for content"""
        try:
            with sqlite3.connect(Config.DATABASE_PATH) as conn:
                conn.execute(
                    'UPDATE content SET last_accessed = datetime(?) WHERE id = ?',
                    (datetime.now().isoformat(), content_id)
                )
                conn.commit()
        except Exception as e:
            logging.error(f"Error updating last accessed for {content_id}: {e}")

    def get_storage_usage(self) -> Dict[str, Any]:
        """Get current storage usage statistics"""
        try:
            total_size = 0
            file_count = 0

            for file_path in Config.CONTENT_DIR.rglob('*'):
                if file_path.is_file():
                    total_size += file_path.stat().st_size
                    file_count += 1

            return {
                'total_size_mb': total_size / (1024 * 1024),
                'file_count': file_count,
                'max_size_mb': Config.MAX_STORAGE_SIZE_MB,
                'usage_percent': (total_size / (Config.MAX_STORAGE_SIZE_MB * 1024 * 1024)) * 100
            }

        except Exception as e:
            logging.error(f"Error getting storage usage: {e}")
            return {'error': str(e)}


class DisplayManager:
    def __init__(self, storage_manager: LocalStorageManager):
        self.storage_manager = storage_manager
        pygame.init()
        pygame.freetype.init()

        # Set up display
        if Config.FULLSCREEN:
            self.screen = pygame.display.set_mode((Config.DISPLAY_WIDTH, Config.DISPLAY_HEIGHT), pygame.FULLSCREEN)
        else:
            self.screen = pygame.display.set_mode((Config.DISPLAY_WIDTH, Config.DISPLAY_HEIGHT))

        pygame.display.set_caption("AIMS Smart Class Display")
        pygame.mouse.set_visible(False)  # Hide mouse cursor

        # Load fonts
        self.font_title = pygame.freetype.SysFont('Arial', Config.FONT_SIZE_TITLE, bold=True)
        self.font_content = pygame.freetype.SysFont('Arial', Config.FONT_SIZE_CONTENT)
        self.font_footer = pygame.freetype.SysFont('Arial', Config.FONT_SIZE_FOOTER)

        self.current_notices = []
        self.current_notice_index = 0
        self.last_notice_change = datetime.now()
        self.running = True
        self.image_cache = {}  # Cache for downloaded images

    def clear_screen(self):
        self.screen.fill(Config.BG_COLOR)

    def draw_text(self, text: str, font, color: tuple, x: int, y: int, max_width: Optional[int] = None):
        """Draw text with optional word wrapping"""
        if max_width:
            # Simple word wrapping
            words = text.split(' ')
            lines = []
            current_line = ""

            for word in words:
                test_line = current_line + " " + word if current_line else word
                if font.get_rect(test_line)[2] <= max_width:
                    current_line = test_line
                else:
                    if current_line:
                        lines.append(current_line)
                    current_line = word
            if current_line:
                lines.append(current_line)

            line_height = font.get_sized_height()
            for i, line in enumerate(lines):
                font.render_to(self.screen, (x, y + i * line_height), line, color)
        else:
            font.render_to(self.screen, (x, y), text, color)

    def display_notice(self, notice: Dict[str, Any]):
        """Display a single notice with local attachments"""
        # Update last accessed timestamp
        if notice.get('_id'):
            self.storage_manager.update_last_accessed(notice['_id'])

        self.clear_screen()

        # Title
        if 'title' in notice:
            self.draw_text(notice['title'], self.font_title, Config.TITLE_COLOR,
                         50, 50, Config.DISPLAY_WIDTH - 100)

        logging.info(f"Displaying notice: {notice.get('title', 'No title')}")
        logging.info(f"Content: {notice.get('content', 'No content')}")
        logging.info(f"Attachments: {len(notice.get('attachments', []))} found")

        # Check for image attachments
        has_image = False
        if 'attachments' in notice and notice['attachments']:
            for attachment in notice['attachments']:
                logging.info(f"Checking attachment: {attachment.get('originalName')} - {attachment.get('mimetype')}")
                if attachment.get('mimetype', '').startswith('image/'):
                    # Construct full URL if it's a relative path
                    image_url = attachment['url']
                    if image_url.startswith('/'):
                        # It's a relative URL, prepend server URL
                        base_url = Config.SERVER_URL.rstrip('/')
                        image_url = f"{base_url}{image_url}"
                    
                    logging.info(f"Downloading image from: {image_url}")
                    image = self.download_image(image_url)
                    if image:
                        # Display image in center area
                        image_y = 150
                        image_height = Config.DISPLAY_HEIGHT - 250  # Leave space for title and footer
                        self.display_image(image, 50, image_y, Config.DISPLAY_WIDTH - 100, image_height)
                        has_image = True
                        logging.info(f"Displayed image: {attachment['originalName']}")
                        break
                    else:
                        logging.error(f"Failed to load image: {attachment['originalName']}")
        else:
            logging.info("No attachments found in notice")

        # Content (only show if no image or if content exists)
        if not has_image and 'content' in notice:
            content_y = 150
            self.draw_text(notice['content'], self.font_content, Config.TEXT_COLOR,
                         50, content_y, Config.DISPLAY_WIDTH - 100)
            logging.info("Displayed text content")
        elif has_image and 'content' in notice and notice['content'].strip():
            # Show content below image if both exist
            content_y = Config.DISPLAY_HEIGHT - 150
            self.draw_text(notice['content'], self.font_content, Config.TEXT_COLOR,
                         50, content_y, Config.DISPLAY_WIDTH - 100)
            logging.info("Displayed text content below image")
        elif has_image:
            logging.info("Displayed image only (no text content)")

        # Display attachments (images)
        attachments = notice.get('local_attachments', [])
        if attachments:
            for attachment in attachments[:3]:  # Show up to 3 images
                try:
                    local_path = attachment.get('localPath', '')
                    if local_path and os.path.exists(local_path):
                        # Load and display image
                        image = pygame.image.load(local_path)
                        # Scale image to fit
                        max_width = Config.DISPLAY_WIDTH - 100
                        max_height = 300
                        img_rect = image.get_rect()
                        scale_factor = min(max_width / img_rect.width, max_height / img_rect.height, 1.0)
                        new_width = int(img_rect.width * scale_factor)
                        new_height = int(img_rect.height * scale_factor)
                        scaled_image = pygame.transform.scale(image, (new_width, new_height))

                        self.screen.blit(scaled_image, (50, content_y))
                        content_y += new_height + 20

                except Exception as e:
                    logging.warning(f"Could not display image {attachment.get('originalName', '')}: {e}")

        # Footer with timestamp and content type
        content_type = "Recurring" if notice.get('is_recurring') else "Limited"
        footer_text = f"Last updated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')} | {content_type}"
        self.draw_text(footer_text, self.font_footer, Config.TEXT_COLOR,
                     50, Config.DISPLAY_HEIGHT - 50)

        pygame.display.flip()

    def download_image(self, url: str) -> Optional[pygame.Surface]:
        """Download and load an image from URL"""
        try:
            logging.info(f"Attempting to download image from: {url}")
            response = requests.get(url, timeout=10)
            response.raise_for_status()
            
            logging.info(f"Image download successful, status: {response.status_code}, content length: {len(response.content)}")
            
            # Load image from bytes
            image = pygame.image.load(io.BytesIO(response.content))
            logging.info(f"Image loaded successfully, size: {image.get_size()}")
            return image
        except requests.RequestException as e:
            logging.error(f"Failed to download image from {url}: {e}")
            return None
        except pygame.error as e:
            logging.error(f"Failed to load image data: {e}")
            return None
        except Exception as e:
            logging.error(f"Unexpected error downloading image: {e}")
            return None

    def display_image(self, image: pygame.Surface, x: int, y: int, max_width: int, max_height: int):
        """Display an image scaled to fit within max dimensions"""
        img_width, img_height = image.get_size()

        # Calculate scaling factor to fit within max dimensions
        scale_x = max_width / img_width
        scale_y = max_height / img_height
        scale = min(scale_x, scale_y, 1.0)  # Don't scale up, only down

        if scale < 1.0:
            new_width = int(img_width * scale)
            new_height = int(img_height * scale)
            image = pygame.transform.smoothscale(image, (new_width, new_height))

        # Center the image
        final_width, final_height = image.get_size()
        center_x = x + (max_width - final_width) // 2
        center_y = y + (max_height - final_height) // 2

        self.screen.blit(image, (center_x, center_y))

    def display_no_content(self):
        """Display when no content is available"""
        self.clear_screen()

        center_x = Config.DISPLAY_WIDTH // 2
        center_y = Config.DISPLAY_HEIGHT // 2

        self.draw_text("No Content Available", self.font_title, Config.TITLE_COLOR,
                     center_x - 200, center_y - 50)

        self.draw_text("Waiting for notices...", self.font_content, Config.TEXT_COLOR,
                     center_x - 150, center_y + 20)

        pygame.display.flip()

    def update_content(self):
        """Update content from local storage"""
        self.current_notices = self.storage_manager.get_active_content()

        if self.current_notices:
            # Sort by priority and reset index
            self.current_notices.sort(key=lambda x: x.get('priority', 0), reverse=True)
            self.current_notice_index = 0
            self.last_notice_change = datetime.now()
            logging.info(f"Loaded {len(self.current_notices)} notices from local storage")
        else:
            self.display_no_content()

    def run(self):
        """Main display loop"""
        clock = pygame.time.Clock()

        while self.running:
            # Handle events
            for event in pygame.event.get():
                if event.type == pygame.QUIT or (event.type == pygame.KEYDOWN and event.key == pygame.K_ESCAPE):
                    self.running = False

            # Update display if we have content
            if self.current_notices:
                # Check if we need to cycle through multiple notices
                now = datetime.now()
                notice_duration = self.current_notices[self.current_notice_index].get('schedule', {}).get('duration', 60)

                if (now - self.last_notice_change).seconds >= notice_duration:
                    # Move to next notice
                    self.current_notice_index = (self.current_notice_index + 1) % len(self.current_notices)
                    self.last_notice_change = now

                # Display current notice
                self.display_notice(self.current_notices[self.current_notice_index])
            else:
                self.display_no_content()

            clock.tick(30)  # 30 FPS

        pygame.quit()

class APIManager:
    def __init__(self, storage_manager: LocalStorageManager):
        self.storage_manager = storage_manager
        self.session = requests.Session()
        self.last_content_update = datetime.now() - timedelta(seconds=Config.CONTENT_UPDATE_INTERVAL)
        self.last_status_update = datetime.now() - timedelta(seconds=Config.STATUS_UPDATE_INTERVAL)

    def get_headers(self) -> Dict[str, str]:
        """Get headers for API requests"""
        headers = {'Content-Type': 'application/json'}
        if Config.API_KEY:
            headers['Authorization'] = f'Bearer {Config.API_KEY}'
        return headers

    def get_board_content(self) -> Optional[Dict[str, Any]]:
        """Fetch board content from the server and store locally"""
        try:
            url = f"{Config.SERVER_URL}/api/boards/{Config.BOARD_ID}/content"
            response = self.session.get(url, headers=self.get_headers(), timeout=10)

            if response.status_code == 200:
                data = response.json()
                if data.get('success'):
                    content_data = data.get('content', {})

                    # Store content locally
                    if self.storage_manager.store_content(content_data):
                        logging.info("Content stored locally successfully")
                    else:
                        logging.warning("Failed to store content locally")

                    return content_data
                else:
                    logging.error(f"API returned error: {data.get('message')}")
            else:
                logging.error(f"HTTP {response.status_code}: {response.text}")

        except requests.RequestException as e:
            logging.error(f"Request failed: {e}")

        return None

    def update_board_status(self, status: str = 'active'):
        """Update board online status"""
        try:
            url = f"{Config.SERVER_URL}/api/boards/{Config.BOARD_ID}/status"
            data = {
                'status': status,
                'lastSeen': datetime.now().isoformat(),
                'isOnline': True
            }
            response = self.session.patch(url, json=data, headers=self.get_headers(), timeout=10)

            if response.status_code == 200:
                logging.info("Board status updated successfully")
            else:
                logging.warning(f"Failed to update board status: HTTP {response.status_code}")

        except requests.RequestException as e:
            logging.error(f"Status update failed: {e}")

class MQTTManager:
    def __init__(self, display_manager):
        self.display_manager = display_manager
        self.client = mqtt.Client()
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message
        self.client.on_disconnect = self.on_disconnect
        self.connected = False

    def on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            logging.info("Connected to MQTT broker")
            self.connected = True
            # Subscribe to notice published topic
            self.client.subscribe(Config.MQTT_TOPIC)
            logging.info(f"Subscribed to topic: {Config.MQTT_TOPIC}")
        else:
            logging.error(f"Failed to connect to MQTT broker: {rc}")

    def on_message(self, client, userdata, msg):
        try:
            payload = json.loads(msg.payload.decode())
            logging.info(f"Received MQTT message on {msg.topic}: {payload}")

            if msg.topic == Config.MQTT_TOPIC:
                # New notice published, update content immediately
                logging.info("Notice published notification received, updating content...")
                api_manager = APIManager()
                content = api_manager.get_board_content()
                if content:
                    self.display_manager.update_content(content)
                    logging.info("Content updated via MQTT notification")

        except Exception as e:
            logging.error(f"Error processing MQTT message: {e}")

    def on_disconnect(self, client, userdata, rc):
        logging.warning(f"Disconnected from MQTT broker: {rc}")
        self.connected = False

    def connect(self):
        try:
            self.client.connect(Config.MQTT_BROKER, Config.MQTT_PORT, 60)
            self.client.loop_start()
            logging.info(f"Attempting to connect to MQTT broker at {Config.MQTT_BROKER}:{Config.MQTT_PORT}")
        except Exception as e:
            logging.error(f"Failed to connect to MQTT broker: {e}")
            logging.info("Falling back to polling-only mode")

    def disconnect(self):
        if self.connected:
            self.client.loop_stop()
            self.client.disconnect()

def setup_logging():
    """Setup logging configuration"""
    logging.basicConfig(
        level=logging.INFO,
        format='%(asctime)s - %(levelname)s - %(message)s',
        handlers=[
            logging.FileHandler('/var/log/raspberry_display.log'),
            logging.StreamHandler(sys.stdout)
        ]
    )

def signal_handler(signum, frame):
    """Handle shutdown signals"""
    logging.info("Shutdown signal received")
    global display_manager
    if display_manager:
        display_manager.running = False

def main():
    global display_manager

    # Setup logging
    setup_logging()
    logging.info("Starting Raspberry Pi Display Client")

    # Validate configuration
    if not Config.BOARD_ID:
        logging.error("BOARD_ID environment variable is required")
        sys.exit(1)

    # Setup signal handlers
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # Initialize components
    storage_manager = LocalStorageManager()
    api_manager = APIManager(storage_manager)
    display_manager = DisplayManager(storage_manager)
    mqtt_manager = MQTTManager(display_manager)

    # Load existing content from local storage on startup
    display_manager.update_content()

    # Connect to MQTT broker for real-time updates
    mqtt_manager.connect()

    # Update board status on startup
    api_manager.update_board_status('active')

    # Start display thread
    display_thread = threading.Thread(target=display_manager.run, daemon=True)
    display_thread.start()

    logging.info("Display client started successfully")

    try:
        while display_manager.running:
            current_time = datetime.now()

            # Update content periodically (try to sync with server)
            if (current_time - api_manager.last_content_update).seconds >= Config.CONTENT_UPDATE_INTERVAL:
                content = api_manager.get_board_content()
                if content:
                    # Update display with fresh content
                    display_manager.update_content()
                    api_manager.last_content_update = current_time
                    logging.info("Content synced from server")
                else:
                    # Server unavailable, refresh from local storage
                    display_manager.update_content()
                    logging.info("Server unavailable, using local content")

            # Update status periodically
            if (current_time - api_manager.last_status_update).seconds >= Config.STATUS_UPDATE_INTERVAL:
                api_manager.update_board_status('active')
                api_manager.last_status_update = current_time

            # Periodic cleanup of expired and old content
            storage_manager.cleanup_expired_content()
            storage_manager.cleanup_old_recurring_content()

            time.sleep(1)

    except KeyboardInterrupt:
        logging.info("Keyboard interrupt received")
    except Exception as e:
        logging.error(f"Unexpected error: {e}")
    finally:
        # Update status on shutdown
        api_manager.update_board_status('offline')
        # Disconnect from MQTT
        mqtt_manager.disconnect()
        logging.info("Display client stopped")

if __name__ == "__main__":
    main()