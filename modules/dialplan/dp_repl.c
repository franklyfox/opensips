/*
 * $Id$
 *
 * Copyright (C) 2007-2008 Voice Sistem SRL
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * History:
 * --------
 *  2007-08-01 initial version (ancuta onofrei)
 */

#include "../../re.h"
#include "dialplan.h"

#define MAX_REPLACE_WITH	10


void repl_expr_free(struct subst_expr *se)
{
	if(!se)
		return;

	if(se->replacement.s){
		shm_free(se->replacement.s);
		se->replacement.s = 0;
	}

	shm_free(se);
	se = 0;
}


struct subst_expr* repl_exp_parse(str subst)
{
	struct replace_with rw[MAX_REPLACE_WITH];
	int rw_no;
	struct subst_expr * se;
	int replace_all;
	char * p, *end, *repl, *repl_end;
	int max_pmatch, r;

	se = 0;
	replace_all = 0;
	p = subst.s;
	end = p + subst.len;
	rw_no = 0;

	repl = p;
	if((rw_no = parse_repl(rw, &p, end, &max_pmatch, WITHOUT_SEP))< 0)
		goto error;
	
	repl_end=p;

	/* construct the subst_expr structure */
	se = shm_malloc(sizeof(struct subst_expr)+
					((rw_no)?(rw_no-1)*sizeof(struct replace_with):0));
		/* 1 replace_with structure is  already included in subst_expr */
	if (se==0){
		LM_ERR("out of shm memory (subst_expr)\n");
		goto error;
	}
	memset((void*)se, 0, sizeof(struct subst_expr));

	se->replacement.len=repl_end-repl;
	if (!(se->replacement.s=shm_malloc(se->replacement.len * sizeof(char))) ){
		LM_ERR("out of shm memory \n");
		goto error;
	}
	if(!rw_no){
		replace_all = 1;
	}
	/* start copying */
	memcpy(se->replacement.s, repl, se->replacement.len);
	se->re=0;
	se->replace_all=replace_all;
	se->n_escapes=rw_no;
	se->max_pmatch=max_pmatch;

	/*replace_with is a simple structure, no shm alloc needed*/
	for (r=0; r<rw_no; r++) se->replace[r]=rw[r];
	return se;

error:
	if (se) { repl_expr_free(se);}
	return NULL;
}


#define MAX_PHONE_NB_DIGITS		127
static char dp_output_buf[MAX_PHONE_NB_DIGITS+1];
int rule_translate(struct sip_msg *msg, str string, dpl_node_t * rule,
		str * result)
{
	int repl_nb, offset, match_nb;
	struct replace_with token;
	TRex * subst_comp;
	struct subst_expr * repl_comp;
	TRexMatch match;
	pv_value_t sv;
	str* uri;

	dp_output_buf[0] = '\0';
	result->s = dp_output_buf;
	result->len = 0;

	subst_comp 	= rule->subst_comp;
	repl_comp 	= rule->repl_comp;

	if(!repl_comp){
		LM_DBG("null replacement\n");
		return 0;
	}

	if(subst_comp){
		/*just in case something went wrong at load time*/
		if((repl_comp->max_pmatch > trex_getsubexpcount(subst_comp))){
			LM_ERR("illegal access to the "
				"%i-th subexpr of the subst expr\n", repl_comp->max_pmatch);
			return -1;
		}

		/*search for the pattern from the compiled subst_exp*/
		if(test_match(string, rule->subst_comp) != 0){
			LM_ERR("the string %.*s "
				"matched the match_exp %.*s but not the subst_exp %.*s!\n", 
				string.len, string.s, 
				rule->match_exp.len, rule->match_exp.s,
				rule->subst_exp.len, rule->subst_exp.s);
			return -1;
		}
	}

	/*simply copy from the replacing string*/
	if(!subst_comp || (repl_comp->n_escapes <=0)){
		if(!repl_comp->replacement.s || repl_comp->replacement.len == 0){
			LM_ERR("invalid replacing string\n");
			goto error;
		}
		LM_DBG("simply replace the string, "
			"subst_comp %p, n_escapes %i\n",subst_comp, repl_comp->n_escapes);
		memcpy(result->s, repl_comp->replacement.s, repl_comp->replacement.len);
		result->len = repl_comp->replacement.len;
		result->s[result->len] = '\0';
		return 0;
	}

	/* offset- offset in the replacement string */
	result->len = repl_nb = offset = 0;
	
	while( repl_nb < repl_comp->n_escapes){
		token = repl_comp->replace[repl_nb];
		
		if(offset< token.offset){
			if((repl_comp->replacement.len < offset)||
				(result->len + token.offset -offset >= MAX_PHONE_NB_DIGITS)){
				LM_ERR("invalid length\n");
				goto error;
			}
			/*copy from the replacing string*/
			memcpy(result->s + result->len, repl_comp->replacement.s + offset, 
					token.offset-offset);
			result->len += (token.offset - offset);
			offset += token.offset-offset; /*update the offset*/
		}

		switch(token.type) {
			case REPLACE_NMATCH:
				/*copy from the match subexpression*/	
				match_nb = token.u.nmatch;
				trex_getsubexp(subst_comp, match_nb, &match);

				if(result->len + match.len >= MAX_PHONE_NB_DIGITS){
					LM_ERR("overflow\n");
					goto error;
				}

				memcpy(result->s + result->len, match.begin, match.len);
				result->len += match.len;
				offset += token.size; /*update the offset*/
				break;

			case REPLACE_CHAR:
				if(result->len + 1>= MAX_PHONE_NB_DIGITS){
					LM_ERR("overflow\n");
					goto error;
				}
				*result->s=repl_comp->replace[repl_nb].u.c;
				result->len++;
				break;
			case REPLACE_URI:
				if ( msg== NULL || msg->first_line.type!=SIP_REQUEST){
					LM_CRIT("uri substitution attempt on no request"
						" message\n");
					break; /* ignore, we can continue */
				}
				uri= (msg->new_uri.s)?(&msg->new_uri):
					(&msg->first_line.u.request.uri);
				if(result->len+uri->len>=MAX_PHONE_NB_DIGITS){
					LM_ERR("overflow\n");
					goto error;
				}
				memcpy(result->s + result->len, uri->s, uri->len);
				result->len+=uri->len;
				break;
			case REPLACE_SPEC:
				if (msg== NULL) {
					LM_DBG("replace spec attempted on no message\n");
					break;
				}
			if(pv_get_spec_value(msg, 
				&repl_comp->replace[repl_nb].u.spec, &sv)!=0){
					LM_CRIT( "item substitution returned error\n");
					break; /* ignore, we can continue */
				}
				if(result->len+sv.rs.len>=MAX_PHONE_NB_DIGITS){
					LM_ERR("ERROR:dialplan: rule_translate: overflow\n");
					goto error;
				}
				memcpy(result->s + result->len, sv.rs.s, sv.rs.len);
				result->len+=sv.rs.len;
				break;
			default:
				LM_CRIT("BUG: unknown type %d\n",
					repl_comp->replace[repl_nb].type);
				/* ignore it */
		}
		repl_nb++;
	}
	/* anything left? */
	if( token.offset+token.size < repl_comp->replacement.len){
		/*copy from the replacing string*/
		memcpy(result->s + result->len,
			repl_comp->replacement.s + token.offset+token.size, 
			repl_comp->replacement.len -(token.offset+token.size) );
			result->len +=repl_comp->replacement.len-(token.offset+token.size);
	}

	result->s[result->len] = '\0';
	return 0;

error:
	result->s = 0;
	result->len = 0;
	return -1;
}

#define DP_MAX_ATTRS_LEN	32
static char dp_attrs_buf[DP_MAX_ATTRS_LEN+1];
int translate(struct sip_msg *msg, str input, str * output, dpl_id_p idp,
																 str * attrs)
{
	dpl_node_p rulep;
	dpl_index_p indexp;
	int user_len, rez;
	
	if(!input.s || !input.len) {
		LM_ERR("invalid input string\n");
		return -1;
	}

	user_len = input.len;
	for(indexp = idp->first_index; indexp!=NULL; indexp = indexp->next)		
		if(!indexp->len || (indexp->len!=0 && indexp->len == user_len) )
			break;

	if(!indexp || (indexp!= NULL && !indexp->first_rule)){
		LM_DBG("no rule for len %i\n", input.len);
		return -1;
	}

search_rule:
	for(rulep=indexp->first_rule; rulep!=NULL; rulep= rulep->next){
		switch(rulep->matchop){

			case REGEX_OP:
				LM_DBG("regex operator testing\n");
				rez = test_match(input, rulep->match_comp);
				break;

			case EQUAL_OP:
				LM_DBG("equal operator testing\n");
				if(rulep->match_exp.len != input.len)
					rez = -1;
				else 
					rez = strncmp(rulep->match_exp.s,input.s,input.len);
				break;

			default:
				LM_ERR("bogus match operator code %i\n", rulep->matchop);
				return -1;
		}
		if(rez==0)
			goto repl;
	}
	/*test the rules with len 0*/
	if(indexp->len){
		for(indexp = indexp->next; indexp!=NULL; indexp = indexp->next)
			if(!indexp->len)
				break;
		if(indexp)
			goto search_rule;
	}
	
	LM_DBG("no matching rule\n");
	return -1;

repl:
	LM_DBG("found a matching rule %p: pr %i, match_exp %.*s\n",
		rulep, rulep->pr, rulep->match_exp.len, rulep->match_exp.s);

	if(attrs){
		attrs->len = 0;
		attrs->s = 0;
		if(rulep->attrs.len>0) {
			LM_DBG("the rule's attrs are %.*s\n",
				rulep->attrs.len, rulep->attrs.s);
			if(rulep->attrs.len >= DP_MAX_ATTRS_LEN) {
				LM_ERR("out of memory for attributes\n");
				return -1;
			}
			attrs->s = dp_attrs_buf;
			memcpy(attrs->s, rulep->attrs.s, rulep->attrs.len*sizeof(char));
			attrs->len = rulep->attrs.len;
			attrs->s[attrs->len] = '\0';

			LM_DBG("the copied attributes are: %.*s\n",
				attrs->len, attrs->s);
		}
	}

	if(rule_translate(msg, input, rulep, output)!=0){
		LM_ERR("could not build the output\n");
		return -1;
	}

	return 0;
}


int test_match(str string, TRex * exp)
{
	const TRexChar *begin,*end;
	TRexMatch match;
	int i, n;
	
	if(!exp){
		LM_ERR("invalid compiled expression\n");
		return -1;
	}

	LM_DBG("test string %.*s against a pattern %s\n", 
		string.len, string.s, exp->_p);

	if(!trex_searchrange(exp, string.s, string.s+ string.len, &begin,&end))
		return -1;
		
	/*if(begin!= string.s || (end != (string.s+string.len)) ){
		LOG(L_ERR, "ERROR:dialplan: test_match:not a perfect match\n");
		return -1;
	}*/

	n = trex_getsubexpcount(exp);

	for(i = 0; i < n; i++){
		trex_getsubexp(exp,i,&match);
		LM_DBG("test_match:[%d] %.*s\n",i,match.len, match.begin);
	}

	return 0;
}

