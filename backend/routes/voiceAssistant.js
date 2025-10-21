﻿const express = require('express');
const { auth, authorize } = require('../middleware/auth');
const { logger } = require('../middleware/logger');
const Device = require('../models/Device');
const ActivityLog = require('../models/ActivityLog');
const {
  createVoiceSession,
  voiceAuth,
  voiceRateLimit,
  revokeVoiceSession,
  getUserVoiceSessions,
  revokeAllUserSessions
} = require('../middleware/voiceAuth');
const {
  handleGoogleAssistantRequest,
  handleAlexaRequest,
  handleSiriRequest,
  processVoiceCommand
} = require('../controllers/voiceAssistantController');

const router = express.Router();

// ============================================
// VOICE SESSION MANAGEMENT (User Authentication)
// ============================================

// Create voice session for authenticated user
router.post('/session/create', auth, async (req, res) => {
  try {
    const user = req.user;

    // Check if user has voice control permissions
    if (user.role === 'student' && !user.permissions?.voiceControl?.enabled) {
      return res.status(403).json({
        success: false,
        message: 'Voice control is not enabled for your account'
      });
    }

    const sessionData = createVoiceSession(user);

    logger.info('[Voice Session] Created for user:', user.name);

    res.json({
      success: true,
      data: sessionData
    });
  } catch (error) {
    logger.error('[Voice Session] Creation error:', error);
    res.status(500).json({
      success: false,
      message: 'Failed to create voice session',
      error: error.message
    });
  }
});

// Get user's active voice sessions
router.get('/session/list', auth, async (req, res) => {
  try {
    const sessions = getUserVoiceSessions(req.user.id);

    res.json({
      success: true,
      data: {
        sessions,
        total: sessions.length
      }
    });
  } catch (error) {
    logger.error('[Voice Session] List error:', error);
    res.status(500).json({
      success: false,
      message: 'Failed to list voice sessions',
      error: error.message
    });
  }
});

// Revoke specific voice session
router.delete('/session/revoke', auth, async (req, res) => {
  try {
    const { voiceToken } = req.body;

    if (!voiceToken) {
      return res.status(400).json({
        success: false,
        message: 'Voice token required'
      });
    }

    const revoked = revokeVoiceSession(voiceToken);

    res.json({
      success: true,
      data: {
        revoked,
        message: revoked ? 'Voice session revoked' : 'Voice session not found'
      }
    });
  } catch (error) {
    logger.error('[Voice Session] Revoke error:', error);
    res.status(500).json({
      success: false,
      message: 'Failed to revoke voice session',
      error: error.message
    });
  }
});

// Revoke all voice sessions for user
router.post('/session/revoke-all', auth, async (req, res) => {
  try {
    const count = revokeAllUserSessions(req.user.id);

    res.json({
      success: true,
      data: {
        revokedCount: count,
        message: `Revoked ${count} voice session(s)`
      }
    });
  } catch (error) {
    logger.error('[Voice Session] Revoke all error:', error);
    res.status(500).json({
      success: false,
      message: 'Failed to revoke voice sessions',
      error: error.message
    });
  }
});

// ============================================
// VOICE COMMAND PROCESSING (Authenticated)
// ============================================

// Voice Command Processing with authentication and rate limiting
router.post('/voice/command', auth, voiceAuth, voiceRateLimit(100, 15 * 60 * 1000), async (req, res) => {
  try {
    const { command, deviceName, switchName, assistant } = req.body;

    logger.info('[Voice Assistant] Authenticated voice command:', {
      user: req.user.name,
      command,
      deviceName,
      switchName,
      assistant
    });

    const result = await processVoiceCommand(command, deviceName, switchName, req.user);

    // Log voice assistant activity with session info
    await ActivityLog.create({
      action: 'voice_command',
      triggeredBy: 'voice_assistant',
      userId: req.user.id,
      userName: req.user.name,
      details: `Voice command: "${command}" via ${assistant || 'web'}`,
      ip: req.ip,
      userAgent: req.get('User-Agent'),
      metadata: {
        assistant: assistant || 'web',
        command,
        deviceName,
        switchName,
        result: result.success,
        sessionInfo: {
          commandCount: req.voiceSession?.commandCount,
          sessionAge: req.voiceSession?.createdAt
        }
      }
    });

    res.json(result);
  } catch (error) {
    logger.error('[Voice Assistant] Voice command error:', error);
    res.status(500).json({
      success: false,
      message: 'Voice command failed',
      error: error.message
    });
  }
});

// ============================================
// VOICE ASSISTANT PLATFORM INTEGRATIONS
// ============================================

// Voice Assistant Integration Routes
// These routes handle requests from Google Assistant, Alexa, and other voice platforms

// Google Assistant Smart Home API
router.post('/google/action', async (req, res) => {
  try {
    const { inputs, requestId } = req.body;

    logger.info('[Voice Assistant] Google Assistant request:', { requestId, inputs });

    const response = await handleGoogleAssistantRequest(inputs, requestId);

    res.json(response);
  } catch (error) {
    logger.error('[Voice Assistant] Google Assistant error:', error);
    res.status(500).json({
      requestId: req.body.requestId || 'unknown',
      payload: {
        errorCode: 'INTERNAL_ERROR',
        debugString: error.message
      }
    });
  }
});

// Alexa Smart Home API
router.post('/alexa/smart-home', async (req, res) => {
  try {
    const { directive } = req.body;

    logger.info('[Voice Assistant] Alexa request:', directive);

    const response = await handleAlexaRequest(directive);

    res.json(response);
  } catch (error) {
    logger.error('[Voice Assistant] Alexa error:', error);
    res.status(500).json({
      event: {
        header: {
          namespace: 'Alexa',
          name: 'ErrorResponse',
          messageId: req.body.directive.header.messageId,
          correlationToken: req.body.directive.header.correlationToken,
          payloadVersion: '3'
        },
        endpoint: {
          endpointId: req.body.directive.endpoint.endpointId
        },
        payload: {
          type: 'INTERNAL_ERROR',
          message: error.message
        }
      }
    });
  }
});

// Siri/HomeKit Integration (webhook-based)
router.post('/siri/webhook', async (req, res) => {
  try {
    const { intent, deviceId, command, parameters } = req.body;

    logger.info('[Voice Assistant] Siri/HomeKit request:', { intent, deviceId, command });

    const response = await handleSiriRequest(intent, deviceId, command, parameters);

    res.json(response);
  } catch (error) {
    logger.error('[Voice Assistant] Siri/HomeKit error:', error);
    res.status(500).json({
      success: false,
      error: error.message
    });
  }
});

// Device Discovery for Voice Assistants (Authenticated)
router.get('/devices/discovery', auth, async (req, res) => {
  try {
    // Get devices accessible to the user based on role and permissions
    let deviceQuery = {};

    if (!['super-admin', 'admin', 'dean'].includes(req.user.role)) {
      // Limited access for non-admin users
      deviceQuery = {
        $or: [
          { assignedUsers: req.user.id },
          { classroom: { $in: req.user.assignedRooms || [] } }
        ]
      };
    }

    const devices = await Device.find(deviceQuery).populate('assignedUsers', 'name');

    const discoveryResponse = {
      google: formatDevicesForGoogle(devices),
      alexa: formatDevicesForAlexa(devices),
      siri: formatDevicesForSiri(devices)
    };

    res.json({
      success: true,
      data: discoveryResponse
    });
  } catch (error) {
    logger.error('[Voice Assistant] Device discovery error:', error);
    res.status(500).json({
      success: false,
      message: 'Device discovery failed',
      error: error.message
    });
  }
});

// Status Reporting for Voice Assistants (Authenticated)
router.get('/devices/:deviceId/status', auth, async (req, res) => {
  try {
    const device = await Device.findById(req.params.deviceId);

    if (!device) {
      return res.status(404).json({ message: 'Device not found' });
    }

    // Check user access to device
    // Add access control logic here

    const status = {
      online: device.status === 'online',
      switches: device.switches.map(sw => ({
        id: sw._id,
        name: sw.name,
        state: sw.state,
        type: sw.type
      })),
      lastSeen: device.lastSeen
    };

    res.json({
      success: true,
      data: status
    });
  } catch (error) {
    logger.error('[Voice Assistant] Status check error:', error);
    res.status(500).json({ message: 'Server error', error: error.message });
  }
});

// Helper functions for device formatting
function formatDevicesForGoogle(devices) {
  return devices.flatMap(device =>
    device.switches.map(switchInfo => ({
      id: `${device._id}_${switchInfo._id}`,
      type: getGoogleDeviceType(switchInfo.type),
      traits: ['action.devices.traits.OnOff'],
      name: {
        name: `${device.name} ${switchInfo.name}`,
        defaultNames: [`${device.location} ${switchInfo.name}`],
        nicknames: [`${device.classroom} ${switchInfo.name}`]
      },
      deviceInfo: {
        manufacturer: 'AutoVolt IoT',
        model: device.deviceType,
        hwVersion: '1.0',
        swVersion: '1.0'
      }
    }))
  );
}

function formatDevicesForAlexa(devices) {
  return devices.flatMap(device =>
    device.switches.map(switchInfo => ({
      endpointId: `${device._id}_${switchInfo._id}`,
      manufacturerName: 'AutoVolt IoT',
      friendlyName: `${device.name} ${switchInfo.name}`,
      description: `${device.location} ${switchInfo.name}`,
      displayCategories: ['SWITCH'],
      capabilities: [
        {
          type: 'AlexaInterface',
          interface: 'Alexa.PowerController',
          version: '3'
        }
      ]
    }))
  );
}

function formatDevicesForSiri(devices) {
  return devices.map(device => ({
    id: device._id,
    name: device.name,
    location: device.location,
    classroom: device.classroom,
    switches: device.switches.map(switchInfo => ({
      id: switchInfo._id,
      name: switchInfo.name,
      type: switchInfo.type
    }))
  }));
}

function getGoogleDeviceType(switchType) {
  const typeMap = {
    'light': 'action.devices.types.LIGHT',
    'fan': 'action.devices.types.FAN',
    'outlet': 'action.devices.types.OUTLET',
    'projector': 'action.devices.types.SWITCH',
    'ac': 'action.devices.types.AC_UNIT'
  };
  return typeMap[switchType] || 'action.devices.types.SWITCH';
}

module.exports = router;

