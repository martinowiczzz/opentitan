// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

use anyhow::Result;
use std::rc::Rc;

use crate::io::gpio::{GpioPin, PinMode, PullMode};
use crate::transport::hyperdebug::Inner;
use crate::transport::TransportError;

pub struct HyperdebugGpioPin {
    inner: Rc<Inner>,
    pinname: String,
}

impl HyperdebugGpioPin {
    pub fn open(inner: &Rc<Inner>, pinname: &str) -> Result<Self> {
        let result = Self {
            inner: Rc::clone(inner),
            pinname: pinname.to_string(),
        };
        Ok(result)
    }
}

impl GpioPin for HyperdebugGpioPin {
    /// Reads the value of the the GPIO pin `id`.
    fn read(&self) -> Result<bool> {
        let line = self
            .inner
            .cmd_one_line_output(&format!("gpioget {}", &self.pinname))
            .map_err(|_| {
                TransportError::CommunicationError("No output from gpioget".to_string())
            })?;
        Ok(line.trim_start().starts_with('1'))
    }

    /// Sets the value of the GPIO pin `id` to `value`.
    fn write(&self, value: bool) -> Result<()> {
        self.inner
            .cmd_no_output(&format!("gpioset {} {}", &self.pinname, u32::from(value)))
    }

    fn set_mode(&self, mode: PinMode) -> Result<()> {
        self.inner.execute_command(
            &format!(
                "gpiomode {} {}",
                &self.pinname,
                match mode {
                    PinMode::Input => "input",
                    PinMode::OpenDrain => "opendrain",
                    PinMode::PushPull => "pushpull",
                    PinMode::Alternate => "alternate",
                }
            ),
            |_| {},
        )
    }

    fn set_pull_mode(&self, mode: PullMode) -> Result<()> {
        self.inner.cmd_no_output(&format!(
            "gpiopullmode {} {}",
            &self.pinname,
            match mode {
                PullMode::None => "none",
                PullMode::PullUp => "up",
                PullMode::PullDown => "down",
            }
        ))
    }
}
