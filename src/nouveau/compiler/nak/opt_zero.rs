// Copyright © 2023 Collabora, Ltd.
// SPDX-License-Identifier: MIT

use crate::{
    api::{GetDebugFlags, DEBUG},
    ir::*,
};

fn try_zero(prev: &mut Instr, this: &Instr) -> Option<RegRef> {
    let Op::Mov(prev_op) = &mut prev.op else {
        return None;
    };
    let Op::Mov(this_op) = &this.op else {
        return None;
    };

    if !prev.srcs()[0].is_zero() || !this.srcs()[0].is_zero() {
        return None;
    }

    let prev_reg = match prev.dsts()[0].as_reg() {
        Some(r) => r,
        None => { return None; }
    };

    let this_reg = match this.dsts()[0].as_reg() {
        Some(r) => r,
        None => { return None; }
    };

    if prev_reg.file() != this_reg.file() {
        return None;
    }

    if prev_reg.base_idx() != this_reg.base_idx() - 1 {
        return None;
    }

    Some(RegRef::new(prev_reg.file(), prev_reg.base_idx(), 2))
}

impl Shader<'_> {
    pub fn opt_zero(&mut self) {
        for f in &mut self.functions {
            for b in &mut f.blocks {
                let mut instrs: Vec<Box<Instr>> = Vec::new();
                for instr in b.instrs.drain(..) {
                    if let Some(prev) = instrs.last_mut() {
                        match try_zero(prev, &instr) {
                            Some(regref) => {
                                instrs.pop();
                                instrs.push(Instr::new_boxed(OpCS2R {
                                    dst: regref.into(), idx: 255
                                }));
                                if DEBUG.annotate() {
                                    instrs.push(Instr::new_boxed(OpAnnotate {
                                        annotation: "combined by opt_zero".into(),
                                    }));
                                }
                                continue;
                            },
                            None => {}
                        }
                    }
                    instrs.push(instr);
                }
                b.instrs = instrs;
            }
        }
    }
}
