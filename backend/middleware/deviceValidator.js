const { body, validationResult } = require('express-validator');

const validateDevice = [
    body('name')
        .trim()
        .notEmpty()
        .withMessage('Device name is required')
        .isLength({ max: 100 })
        .withMessage('Device name must be less than 100 characters'),
    
    body('macAddress')
        .trim()
        .notEmpty()
        .withMessage('MAC address is required')
        .matches(/^([0-9A-Fa-f]{2}[:-]){5}([0-9A-Fa-f]{2})$/)
        .withMessage('Invalid MAC address format'),

    body('ipAddress')
        .trim()
        .notEmpty()
        .withMessage('IP address is required')
        .matches(/^(\d{1,3}\.){3}\d{1,3}$/)
        .withMessage('Invalid IP address format')
        .custom(val => {
            const parts = val.split('.');
            if (!parts.every(p => Number(p) >= 0 && Number(p) <= 255)) {
                throw new Error('Each IP octet must be between 0 and 255');
            }
            return true;
        }),
    
    body('location')
        .trim()
        .notEmpty()
        .withMessage('Location is required'),
    
    body('pirGpio')
        .optional()
        .isInt({ min: 0, max: 39 })
        .withMessage('GPIO pin must be between 0 and 39')
        .custom(value => {
            if ([6, 7, 8, 9, 10, 11].includes(value)) {
                throw new Error('This GPIO pin is reserved for internal use');
            }
            return true;
        }),
    
    body('switches')
        .isArray()
        .withMessage('Switches must be an array')
        .custom(switches => {
            switches.forEach((sw, index) => {
                if (!sw.name) {
                    throw new Error(`Switch ${index + 1} name is required`);
                }
                // GPIO is optional for switches
                if (sw.gpio !== undefined && (typeof sw.gpio !== 'number' || sw.gpio < 0 || sw.gpio > 39)) {
                    throw new Error(`Switch ${index + 1} has invalid GPIO pin`);
                }
                if (sw.gpio !== undefined && [6, 7, 8, 9, 10, 11].includes(sw.gpio)) {
                    throw new Error(`Switch ${index + 1} uses reserved GPIO pin`);
                }
                // manualSwitchGpio is also optional
                if (sw.manualSwitchGpio !== undefined && (typeof sw.manualSwitchGpio !== 'number' || sw.manualSwitchGpio < 0 || sw.manualSwitchGpio > 39)) {
                    throw new Error(`Switch ${index + 1} has invalid manual switch GPIO pin`);
                }
                if (sw.manualSwitchGpio !== undefined && [6, 7, 8, 9, 10, 11].includes(sw.manualSwitchGpio)) {
                    throw new Error(`Switch ${index + 1} uses reserved manual switch GPIO pin`);
                }
            });
            return true;
        }),

    (req, res, next) => {
        const errors = validationResult(req);
        if (!errors.isEmpty()) {
            return res.status(400).json({
                error: 'Validation failed',
                details: errors.array()
            });
        }
        next();
    }
];

const validateDeviceUpdate = [
    body('name')
        .optional()
        .trim()
        .notEmpty()
        .withMessage('Device name cannot be empty')
        .isLength({ max: 100 })
        .withMessage('Device name must be less than 100 characters'),
    
    body('macAddress')
        .optional()
        .trim()
        .notEmpty()
        .withMessage('MAC address cannot be empty')
        .matches(/^([0-9A-Fa-f]{2}[:-]){5}([0-9A-Fa-f]{2})$/)
        .withMessage('Invalid MAC address format'),

    body('ipAddress')
        .optional()
        .trim()
        .notEmpty()
        .withMessage('IP address cannot be empty')
        .matches(/^(\d{1,3}\.){3}\d{1,3}$/)
        .withMessage('Invalid IP address format')
        .custom(val => {
            const parts = val.split('.');
            if (!parts.every(p => Number(p) >= 0 && Number(p) <= 255)) {
                throw new Error('Each IP octet must be between 0 and 255');
            }
            return true;
        }),
    
    body('location')
        .optional()
        .trim()
        .notEmpty()
        .withMessage('Location cannot be empty'),
    
    body('pirGpio')
        .optional()
        .isInt({ min: 0, max: 39 })
        .withMessage('GPIO pin must be between 0 and 39')
        .custom(value => {
            if ([6, 7, 8, 9, 10, 11].includes(value)) {
                throw new Error('This GPIO pin is reserved for internal use');
            }
            return true;
        }),
    
    body('switches')
        .optional()
        .isArray()
        .withMessage('Switches must be an array')
        .custom(switches => {
            switches.forEach((sw, index) => {
                if (!sw.name) {
                    throw new Error(`Switch ${index + 1} name is required`);
                }
                // GPIO is optional for switches
                if (sw.gpio !== undefined && (typeof sw.gpio !== 'number' || sw.gpio < 0 || sw.gpio > 39)) {
                    throw new Error(`Switch ${index + 1} has invalid GPIO pin`);
                }
                if (sw.gpio !== undefined && [6, 7, 8, 9, 10, 11].includes(sw.gpio)) {
                    throw new Error(`Switch ${index + 1} uses reserved GPIO pin`);
                }
                // manualSwitchGpio is also optional
                if (sw.manualSwitchGpio !== undefined && (typeof sw.manualSwitchGpio !== 'number' || sw.manualSwitchGpio < 0 || sw.manualSwitchGpio > 39)) {
                    throw new Error(`Switch ${index + 1} has invalid manual switch GPIO pin`);
                }
                if (sw.manualSwitchGpio !== undefined && [6, 7, 8, 9, 10, 11].includes(sw.manualSwitchGpio)) {
                    throw new Error(`Switch ${index + 1} uses reserved manual switch GPIO pin`);
                }
            });
            return true;
        }),

    (req, res, next) => {
        const errors = validationResult(req);
        if (!errors.isEmpty()) {
            return res.status(400).json({
                error: 'Validation failed',
                details: errors.array()
            });
        }
        next();
    }
];

module.exports = { validateDevice, validateDeviceUpdate };
